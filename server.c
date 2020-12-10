#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "./comm.h"
#include "./db.h"

/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;
    pthread_cond_t server_cond;
    int num_client_threads;
    int is_open;
} server_control_t;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*
 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;

client_t *thread_list_head = NULL;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

server_control_t s_controller = {PTHREAD_MUTEX_INITIALIZER,
                                 PTHREAD_COND_INITIALIZER, 0, 1};

client_control_t c_controller = {PTHREAD_MUTEX_INITIALIZER,
                                 PTHREAD_COND_INITIALIZER, 0};

void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);

// Called by client threads to wait until progress is permitted
void client_control_wait() {
    // TODO: Block the calling thread until the main thread calls
    // client_control_release(). See the client_control_t struct.
    int err = pthread_mutex_lock(&c_controller.go_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_lock");
    }
    pthread_cleanup_push((void(*))pthread_mutex_unlock,
                         &(c_controller.go_mutex));
    while (c_controller.stopped == 1) {
        pthread_cond_wait(&c_controller.go, &c_controller.go_mutex);
        if (err != 0) {
            handle_error_en(err, "pthread_cond_wait");
        }
    }
    pthread_cleanup_pop(1);
}

// Called by main thread to stop client threads
void client_control_stop() {
    // TODO: Ensure that the next time client threads call client_control_wait()
    // at the top of the event loop in run_client, they will block.
    int err = pthread_mutex_lock(&c_controller.go_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_lock");
    }
    c_controller.stopped = 1;
    err = fprintf(stderr, "stopping all clients\n");
    if (err < 0) {
        perror("fprintf");
    }
    err = pthread_mutex_unlock(&c_controller.go_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_unlock");
    }
}

// Called by main thread to resume client threads
void client_control_release() {
    // TODO: Allow clients that are blocked within client_control_wait()
    // to continue. See the client_control_t struct.
    int err;
    err = pthread_mutex_lock(&c_controller.go_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_lock");
    }
    c_controller.stopped = 0;
    err = fprintf(stderr, "releasing all clients\n");
    if (err < 0) {
        perror("fprintf");
    }
    err = pthread_cond_broadcast(&c_controller.go);
    if (err != 0) {
        handle_error_en(err, "pthread_cond_wait");
    }
    err = pthread_mutex_unlock(&c_controller.go_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_unlock");
    }
}

// Called by listener (in comm.c) to create a new client thread
void client_constructor(FILE *cxstr) {
    // You should create a new client_t struct here and initialize ALL
    // of its fields. Remember that these initializations should be
    // error-checked.
    //
    // TODO:
    int err;
    // Step 1: Allocate memory for a new client and set its connection stream
    // to the input argument.
    struct client *p = malloc(sizeof(struct client));
    if (p == NULL) {
        exit(1);
    }
    p->cxstr = cxstr;
    p->prev = NULL;
    p->next = NULL;
    // Step 2: Create the new client thread running the run_client routine.
    err = pthread_create(&(p->thread), 0, run_client, p);
    if (err != 0) {
        handle_error_en(err, "pthread_create");
    }
    // Step 3: Detach the new client thread
    err = pthread_detach(p->thread);
    if (err != 0) {
        handle_error_en(err, "pthread_detach");
    }
}

// called by thread_cleanup. Shuts down the socket and frees client memory
void client_destructor(client_t *client) {
    // TODO: Free and close all resources associated with a client.
    // Whatever was malloc'd in client_constructor should
    // be freed here!
    comm_shutdown(client->cxstr);
    free(client);
}

// Code executed by a client thread
void *run_client(void *arg) {
    // TODO:
    // Step 1: Make sure that the server is still accepting clients.
    struct client *c = (struct client *)arg;
    if (s_controller.is_open) {
        int err;
        // Step 2: Add client to the client list and push thread_cleanup to
        // remove it if the thread is canceled.

        c->prev = NULL;
        err = pthread_mutex_lock(&thread_list_mutex);
        if (err != 0) {
            handle_error_en(err, "pthread_mutex_lock");
        }
        c->next = thread_list_head;
        if (thread_list_head != NULL) {
            thread_list_head->prev = c;
        }
        thread_list_head = c;
        err = pthread_mutex_unlock(&thread_list_mutex);
        if (err != 0) {
            handle_error_en(err, "pthread_mutex_unlock");
        }

        err = pthread_mutex_lock(&s_controller.server_mutex);
        if (err != 0) {
            handle_error_en(err, "pthread_mutex_lock");
        }
        s_controller.num_client_threads++;
        err = pthread_mutex_unlock(&s_controller.server_mutex);
        if (err != 0) {
            handle_error_en(err, "pthread_mutex_unlock");
        }

        pthread_cleanup_push(thread_cleanup, c);
        // Step 3: Loop comm_serve (in comm.c) to receive commands and output
        //       responses. Execute commands using interpret_command (in db.c)
        char response[256];
        char command[256];
        response[0] = 0;
        while (comm_serve(c->cxstr, response, command) == 0) {
            client_control_wait();
            interpret_command(command, response, 256);
        }
        pthread_cleanup_pop(1);
    }
    // Step 4: When the client is done sending commands, exit the thread
    //       cleanly.
    pthread_exit(0);
    //
    // Keep stop and go in mind when writing this function!
    return NULL;
}

// sends a cancellation signal to each thread in the thread list
void delete_all() {
    // TODO: Cancel every thread in the client thread list with the
    // pthread_cancel function.
    int err;
    err = pthread_mutex_lock(&thread_list_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_lock");
    }
    for (client_t *client = thread_list_head; client != NULL;
         client = client->next) {
        err = pthread_cancel(thread_list_head->thread);
        if (err != 0) {
            handle_error_en(err, "pthread_cancel");
        }
    }
    err = pthread_mutex_unlock(&thread_list_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_unlock");
    }
}

// Cleanup routine for client threads, called on cancels and exit.
void thread_cleanup(void *arg) {
    // TODO: Remove the client object from thread list and call
    // client_destructor. This function must be thread safe! The client must
    // be in the list before this routine is ever run.
    client_t *c = (client_t *)arg;
    int err;
    err = pthread_mutex_lock(&thread_list_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_lock");
    }
    client_t *n = c->next;
    client_t *p = c->prev;
    if (n != NULL) {
        n->prev = p;
    }

    if (c == thread_list_head) {
        thread_list_head = c->next;
    } else {
        if (p != NULL) {
            p->next = n;
        }
    }
    err = pthread_mutex_unlock(&thread_list_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_unlock");
    }

    // decrement thread counter
    err = pthread_mutex_lock(&s_controller.server_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_lock");
    }
    s_controller.num_client_threads--;
    if (s_controller.num_client_threads == 0) {
        err = pthread_cond_signal(&s_controller.server_cond);
        if (err != 0) {
            handle_error_en(err, "pthread_cond_signal");
        }
    }
    err = pthread_mutex_unlock(&s_controller.server_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_mutex_unlock");
    }
    client_destructor(c);
}

// Code executed by the signal handler thread. For the purpose of this
// assignment, there are two reasonable ways to implement this.
// The one you choose will depend on logic in sig_handler_constructor.
// 'man 7 signal' and 'man sigwait' are both helpful for making this
// decision. One way or another, all of the server's client threads
// should terminate on SIGINT. The server (this includes the listener
// thread) should not, however, terminate on SIGINT!
void *monitor_signal(void *arg) {
    // TODO: Wait for a SIGINT to be sent to the server process and cancel
    // all client threads when one arrives.
    int sig;
    sig_handler_t *sh = (sig_handler_t *)arg;
    while (1) {
        int err = sigwait(&(sh->set), &sig);
        if (err != 0) {
            exit(1);
        }
        fprintf(stdout, "SIGINT received, cancelling all clients\n");
        delete_all();
    }
    return NULL;
}

// creates the thread that handles signals
sig_handler_t *sig_handler_constructor() {
    // TODO: Create a thread to handle SIGINT. The thread that this function
    // creates should be the ONLY thread that ever responds to SIGINT.
    sig_handler_t *sh = malloc(sizeof(sig_handler_t));

    int err;
    err = sigemptyset(&(sh->set));
    if (err == -1) {
        exit(1);
    }
    err = sigaddset(&(sh->set), SIGINT);
    if (err == -1) {
        exit(1);
    }
    err = pthread_sigmask(SIG_BLOCK, &(sh->set), 0);
    if (err != 0) {
        handle_error_en(err, "pthread_sigmask");
    }
    err = pthread_create(&(sh->thread), 0, monitor_signal, sh);
    if (err != 0) {
        handle_error_en(err, "pthread_create");
    }
    return sh;
}

// destroys the signal-handler thread by cancelling, joining, and freeing
void sig_handler_destructor(sig_handler_t *sighandler) {
    // TODO: Free any resources allocated in sig_handler_constructor.
    // Cancel and join with the signal handler's thread.
    int err = pthread_cancel(sighandler->thread);
    if (err != 0) {
        handle_error_en(err, "pthread_cancel");
    }
    err = pthread_join(sighandler->thread, NULL);
    if (err != 0) {
        handle_error_en(err, "pthread_join");
    }
    free(sighandler);
}

// The arguments to the server should be the port number.
int main(int argc, char *argv[]) {
    int err;
    if (argc != 2) {
        fprintf(stderr, "Usage: port\n");
        exit(1);
    }
    int port = atoi(argv[1]);
    // TODO:
    // Step 1: Set up the signal handler.
    sig_handler_t *sh = sig_handler_constructor();
    // Step 2: ignore SIGPIPE so that the server does not abort when a client
    // disconnects
    signal(SIGPIPE, SIG_IGN);

    // Step 3: Start a listener thread for clients (see start_listener in
    //       comm.c).
    pthread_t l_tid =
        start_listener(port, ((void (*)(FILE *))client_constructor));

    // Step 4: Loop for command line input and handle accordingly until EOF.
    char line[256];
    char *fgets_ret;

    while (1) {
        fgets_ret = fgets(line, sizeof(line), stdin);
        if (fgets_ret == NULL) {
            break;
        }

        char cmd[2];
        cmd[0] = line[0];
        cmd[1] = '\0';
        char *dest = line + 1;
        char *token = strtok(dest, " \n\t");
        if (strcmp(cmd, "s") == 0) {
            client_control_stop();
        } else if (strcmp(cmd, "g") == 0) {
            client_control_release();
        } else if (strcmp(cmd, "p") == 0) {
            db_print(token);
        }
    }
    // Step 5: Destroy the signal handler, delete all clients, cleanup the
    //       database, cancel and join with the listener thread
    //
    sig_handler_destructor(sh);
    delete_all();
    err = pthread_mutex_lock(&s_controller.server_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_lock");
    }
    s_controller.is_open = 0;
    while (s_controller.num_client_threads != 0) {
        pthread_cond_wait(&s_controller.server_cond,
                          &s_controller.server_mutex);
        if (err != 0) {
            handle_error_en(err, "pthread_cond_wait");
        }
    }
    pthread_mutex_unlock(&s_controller.server_mutex);
    if (err != 0) {
        handle_error_en(err, "pthread_unlock");
    }
    assert(thread_list_head == NULL);
    fprintf(stdout, "exiting database\n");
    db_cleanup();

    err = pthread_cancel(l_tid);
    if (err != 0) {
        handle_error_en(err, "pthread_cancel");
    }
    err = pthread_join(l_tid, NULL);
    if (err != 0) {
        handle_error_en(err, "pthread_join");
    }
    // You should ensure that the thread list is empty before cleaning up the
    // database and canceling the listener thread. Think carefully about what
    // happens in a call to delete_all() and ensure that there is no way for a
    // thread to add itself to the thread list after the server's final
    // delete_all().
    return 0;
}