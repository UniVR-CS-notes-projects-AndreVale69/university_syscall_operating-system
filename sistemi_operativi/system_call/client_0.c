/// @file client.c
/// @brief Contiene l'implementazione del client.

#include "defines.h"
#include "err_exit.h"
#include "fifo.h"
#include "semaphore.h"
#include "shared_memory.h"

// set of signals
sigset_t original_set_signals;
sigset_t unlocked_set_signals;
sigset_t blocked_set_signals;

// manipulation of a signal & mod signal mask
void sigHandler(int sig);
void set_original_mask();
void create_signal_mask();
int fifo1_fd;
int fifo2_fd;
int queue_id;
int shmem_id;
struct queue_msg *shmpointer;
pid_t client_pid;

int main(int argc, char * argv[]) {

    // Init variables
    int semid;
    pid_t pid = 1;
    int i;
    client_pid = getpid();              // get the current pid to send to server
    int count = 0;                      // file found counter
    char to_send[MAX_FILES][PATH];      // Creating an array to store the file paths

    /**************
     * CHECK ARGS *
     **************/

    if (argc != 2)
        errExit("Intended usage: ./client_0 <HOME>/myDir/");
    
    // pointer to the inserted path
    char *path_to_dir = argv[1];
    char buf[PATH];

    // try to obtain set id of semaphores
    // if the client is unable to find it, we leave the original masks so the user could kill the process with a ctrl + c 
    do {
        printf("Looking for the semaphore...\n\n");
        semid = semget(ftok("client_0", 'a'), 0, S_IRUSR | S_IWUSR);
        if (errno == ENOENT)
            sleep(2);
        else if(semid == -1){
            errExit("Error while retrieving the semaphore");
        }
    } while(semid == -1);

    create_signal_mask();

    /*****************
     * OBTAINING IDs *
     *****************/

    // waiting for IPCs to be created
    semop_usr(semid, 0, -1);
    
    // opening of all the IPC's
    fifo1_fd = open_fifo("FIFO1", O_WRONLY);
    fifo2_fd = open_fifo("FIFO2", O_WRONLY);
    queue_id = msgget(ftok("client_0", 'a'), S_IRUSR | S_IWUSR);
    shmem_id = alloc_shared_memory(ftok("client_0", 'a'), sizeof(struct queue_msg) * 50, S_IRUSR | S_IWUSR);
    shmpointer = (struct  queue_msg *) attach_shared_memory(shmem_id, 0);

    // change process working directory
    if (chdir(path_to_dir) == -1)
        errExit("Error while changing directory");

    while(1){      

        // Set the count to 0 everytime so the recursive function works allright
        count = 0;

        // blocking all signals except:
        // SIGKILL, SIGSTOP (default)
        // SIGINT, SIGUSR1
        if(sigprocmask(SIG_SETMASK, &unlocked_set_signals, NULL) == -1)
            errExit("sigprocmask(new_set) failed!");
        
        // definition manipulate SIGINT
        if (signal(SIGINT, sigHandler) == SIG_ERR)
            errExit("change signal handler (SIGINT) failed!");

        // definition manipulate SIGUSR1
        if (signal(SIGUSR1, sigHandler) == SIG_ERR)
            errExit("change signal handler (SIGUSR1) failed!");

        printf("Ready to go! press ctrl + c\n");

        // waiting for a signal...
        pause();

        // blocking all blockable signals
        if(sigprocmask(SIG_SETMASK, &blocked_set_signals, NULL) == -1)
            errExit("sigprocmask(original_set) failed");

        /****************
         * FILE READING *
         ****************/
        
        // get current working directory
        getcwd(buf, MAX_LENGTH_PATH);

        printf("Ciao %s, ora inizio l’invio dei file contenuti in %s\n\n", getenv("USER"), buf);

        // search files into directory
        count = search_dir (buf, to_send, count);


        /*******************************
         * CLIENT-SERVER COMMUNICATION *
         *******************************/

        // Writing n_files on FIFO1
        write_fifo(fifo1_fd, &count, sizeof(count));
        write_fifo(fifo1_fd, &client_pid, sizeof(pid_t));

        // Unlocking semaphore 1 (allow server to read from FIFO1)
        semop_usr(semid, 1, 1);

        //Wait for server to send "READY", 
        //waiting ShdMem semaphore unlock by server 
        semop_usr(semid, 4, -1);

        if(strcmp(shmpointer[0].fragment, "READY") != 0){
            errExit("Corrupted start message");
        }

        semop_usr(semid, 0, count);

        //Child creation, parent operations
        for(i = 0; i < count && pid != 0; i++){
            pid = fork();
            if(pid == -1)
                errExit("Error while forking");
        }

        //child operations
        if(pid == 0){
            semop_usr(semid, 0, -1);
            semop_usr(semid, 0, 0);
            exit(0);
        } else{

            // Waiting for all child to terminate
            for(int j = 0; j < count; j++){
                if(wait(NULL) == -1)
                    errExit("Error while waiting for children");
            }
        }

        // Let the server know that we are done
        semop_usr(semid, 5, -1);
    }

    return 0;
}




// gimmy attento che qui finisce la main!!!




    /******************************
    * BEGIN FUNCTIONS DEFINITIONS *
    *******************************/

// Personalised signal handler for SIGUSR1 and SIGINT
void sigHandler (int sig) {
    // if signal is SIGUSR1, set original mask and kill process
    if(sig == SIGUSR1) {
        set_original_mask();

        /**************
        * CLOSE IPCs *
        **************/ 

        close(fifo1_fd);
        close(fifo2_fd);
        free_shared_memory(shmpointer);
        exit(0);
    }

    // if signal is SIGINT, happy(end) continue
    if(sig == SIGINT)
        printf("\n\nI'm awake!\n\n");
}

// function used to reset the default mask of the process
void set_original_mask() {
    // reset the signal mask of the process = restore original mask
    if(sigprocmask(SIG_SETMASK, &original_set_signals, NULL) == -1)
        errExit("sigprocmask(original_set) failed");
}

void create_signal_mask() {

    // Initialize a set with all the signals
    if(sigfillset(&blocked_set_signals) == -1)
        errExit("sigfillset failed!");

    // initialize unlocked_set_signals to contain all signals of OS
    if(sigfillset(&unlocked_set_signals) == -1)
        errExit("sigfillset failed!");

    // remove SIGINT from unlocked_set_signals
    if(sigdelset(&unlocked_set_signals, SIGINT) == -1)
        errExit("sigdelset(SIGINT) failed!");
    // remove SIGUSR1 from unlocked_set_signals
    if(sigdelset(&unlocked_set_signals, SIGUSR1) == -1)
        errExit("sigdelset(SIGUSR1) failed!");
    
    if(sigprocmask(SIG_SETMASK, &unlocked_set_signals, &original_set_signals) == -1)
        errExit("sigprocmask(new_set) failed!");
    
}