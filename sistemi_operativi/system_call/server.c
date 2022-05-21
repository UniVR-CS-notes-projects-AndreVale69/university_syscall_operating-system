/// @file sender_manager.c
/// @brief Contiene l'implementazione del sender_manager.

#include "err_exit.h"
#include "defines.h"
#include "shared_memory.h"
#include "semaphore.h"
#include "fifo.h"
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <unistd.h>
#include <fcntl.h> 
#include <sys/shm.h>
#include <stdio.h>

// Declaration of functions
void sigHandler (int sig);

//init variables
int semid;
int queue_id;
int fifo1_fd;
int fifo2_fd;
int shmem_id;
int n_files;    // files to read from FIFO
ssize_t num_read;
struct queue_msg *shmpointer;
union semun semarg;
pid_t client_pid;
int opened;


int main(int argc, char * argv[]) {

    struct queue_msg packet;

    // set flag to zero (never say never)
    opened = 0;

    // sem order: Access, FIFO1, FIFO2, MsgQueue, ShdMem, FINISH_CLIENT, FINISH_SERVER
    unsigned short semarray[7] = {0, 0, 1, 1, 0, 1, 1};
    semarg.array = semarray;

    // creation of all the semaphores
    semid = semget_usr(ftok("client_0", 'a'), 7, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

    if (semctl(semid, 0, SETALL, semarg) == -1)
        errExit("Error while initializing semaphore set");

    // creation of all IPC's
    create_fifo("FIFO1");
    create_fifo("FIFO2");
    queue_id = msgget(ftok("client_0", 'a'), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if(queue_id == -1)
        errExit("Error while creating a message queue");
    shmem_id = alloc_shared_memory(ftok("client_0", 'a'),
                                    sizeof(struct queue_msg) * IPC_MAX,
                                    IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);

    // set sigHandler as a handler for the SIGINT
    if (signal(SIGINT, sigHandler) == SIG_ERR)
        errExit("change signal handler (SIGINT) failed!");

    // opening and attaching all of the IPC
    fifo1_fd = open_fifo("FIFO1", O_RDONLY | O_NONBLOCK);
    fifo2_fd = open_fifo("FIFO2", O_RDONLY | O_NONBLOCK);
    shmpointer = (struct  queue_msg *) attach_shared_memory(shmem_id, 0);

    // unlocking semaphore ACCESS (all IPCs have been created)
    semop_usr(semid, ACCESS, 1);

    printf("Server ready!\n");

    // wait for client to open IPCs
    semop_usr(semid, FINISH_CLIENT, -2);

    // set flag to one
    opened = 1;

    while(1) {

        // reset all the semaphores to restart the cicle and unlock Access (all IPCs have been created)
        // Access, FIFO1, FIFO2, MsgQueue, ShdMem, FINISH_CLIENT, FINISH_SERVER
        // 0,      0,     1,     1,        0,      1,             1
        if (semctl(semid, 0, SETALL, semarg) == -1)
            errExit("Error while initializing semaphore set");

        printf("Il valore di FINISH_SERVER: %d\n", semctl(semid, FINISH_SERVER, GETVAL));

        // lock first semaphore until the number of files are written on FIFO1
        printf("\nWaiting for client...\n\n");
        semop_usr(semid, FIFO1, -1);

        // retrieve n_files from FIFO1 and then retrieve client PID
        read_fifo(fifo1_fd, &n_files, sizeof(int));
        read_fifo(fifo1_fd, &client_pid, sizeof(pid_t));

        strcpy(shmpointer[0].fragment, "READY\0");
        semop_usr(semid, SHDMEM, 2);

        printf("Numeri di file da elaborare: %d\n", n_files);

        for(int i = 0; i < n_files * 4;) {

            // Retrieve message from FIFO1
            read_fifo(fifo1_fd, &packet, sizeof(packet));
            if (errno == 0) {
                printf("FIFO1 process PID: %d, Message: %s\n", packet.pid, packet.fragment);
                semop_usr(semid, FIFO1, 1);
                i++;
            }

            // Retrieve message from FIFO2
            read_fifo(fifo2_fd, &packet, sizeof(packet));
            if (errno == 0) {
                printf("FIFO2 process PID: %d, Message: %s\n", packet.pid, packet.fragment);
                semop_usr(semid, FIFO2, 1);
                i++;
            }

            // Retrieve message from MSGQUEUE
            errno = 0;
            msgrcv(queue_id, &packet, sizeof(packet), 0, IPC_NOWAIT);
            if (errno == 0) {
                printf("MSGQUEUE process PID: %d, Message: %s\n", packet.pid, packet.fragment);
                semop_usr(semid, MSGQUEUE, 1);
                i++;
            }
            else if (errno != ENOMSG) {
                errExit("Error while receiving message");
            }

            // Retrieve message from SHMEM
            semop_nowait(semid, SHDMEM, -1);
            if(errno == 0){
                for(int k = 0, read = 0; k < IPC_MAX && read == 0; k++){
                    if(shmpointer[k].mtype != 0){
                        printf("SHDMEM process PID: %d, Message: %s\n", shmpointer[k].pid, shmpointer[k].fragment);
                        shmpointer[k].mtype = 0;
                        read++;
                        i++;
                    }
                }
                semop_nowait(semid, SHDMEM, 1);
            }

        }

        printf("Il valore di FINISH_SERVER: %d\n", semctl(semid, FINISH_SERVER, GETVAL));

        printf("PORCO\n");
        // let client know that we are done
        semop_usr(semid, FINISH_SERVER, 1);
        printf("DIO\n");
        // wait for client to finish
        semop_usr(semid, FINISH_CLIENT, -1);
    }
}

void sigHandler (int sig) {
    // when SIGINT is caught, close everything and send SIGUSR1 to client
    if(sig == SIGINT) {
        printf("\nShutdown server...\n");

        if (opened) {
            // close IPCs
            close_fifo(fifo1_fd);
            close_fifo(fifo2_fd);
            free_shared_memory(shmpointer);
        }

        unlink("FIFO1");
        unlink("FIFO2");
        if(msgctl(queue_id, IPC_RMID, NULL) == -1)
            errExit("Error while removing the message queue");
        if(semctl(semid, 0, IPC_RMID, NULL) == -1)
            errExit("Error while removing the message queue");
        remove_shared_memory(shmem_id);

        // send SIGUSR1 to terminate client_0
        if(kill(client_pid, SIGUSR1) == -1)
            errExit("Error while sending SIGUSR1 to server");

        exit(0);
    }
}