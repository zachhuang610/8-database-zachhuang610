# 8-database

PROGRAM STRUCTURE:

server.c:
                            CLIENT SYNC CONTROL
    client_control_wait: function for clients to wait for a "go" signal from the server
    client_control_stop: function for the server to send a stop signal, preventing new
            clients from running "interpret command" until a "go" signal has been sent
    client_control_release: broadcasts a "go" signal

                            CLIENT CREATION
    client_constructor: called by listener in start_listener to create new threads. Mallocs
            new memory for the client, then creates a new thread and then calls run_client 
            in them. then detaches.
    client_destructor: called at the end of thread_cleanup. Calls comm_shutdown and frees 
            the malloc'd client memory. 
    run_client: executed by the client thread, responsible for adding to the threadlist in a
            thread-safe way, and then calling comm_serve, waiting to make sure "go" signal is 
            being broadcasted, and then interpreting until there is an EOF. pushs 
            thread_cleanup before and pops after. Exits the thread when the client is done
            sending commands.
    delete_all: cancels every thread in the threadlist in a thread-safe manner. 
    thread_cleanup: cancellation routine when any pthread_cancel is called on a client thread.
            removes the object from the threadlist and decrements the thread counter in the
            server_control_t object. If the thread being cancelled is the last thread in the 
            server, a signal is broadcasted making it safe for database cleanup to occur. 
            Then calls client_destructor to free the memory and shutdown the socket.
    
                            SIGNAL HANDLING
    monitor_signal: called in a signal handling thread that waits until SIGINT is received then
            calls delete_all to cancel all threads.
    sig_handler_constructor: creates the sig handler object, adds SIGINT to the mask, creates a
            new thread for handling SIGINTs and calls monitor_signal in that thread with the 
            created sigset.
    sig_handler_destructor: cancels the signal handling thread, joins, and frees the object.



db.c:
                            NODE MANAGEMENT:
    node_constructor: creates a node, made threadsafe by locking the created node
    node_destructor: unlocks the node before destroying the lock and freeing it.
    lock: function for locking a node in either read or write.
    unlock: function fro unlocking a node

                            BST FUNCTIONS:
    search: function for searching in the tree. Made threadsafe using hand-over-hand method.
    db_query: function for getting a value, made thread_safe by making search thread-safe
    db_add: function for adding a value, made threadsafe by making search threadsafe
    db_remove: made thread-safe by locking nodes we are reading or modifying. A hand-over-hand
                locking mechanism is used in cases where target node has both rchild and lchild
    db_print_recur: functionally does coarse-grained locking unlike other BST functions. Locks
                each node on entry and unlocks on return after printing.
    db_print: locks the head before calling db_print_recur to ensure thread-safety



PROGRAM FUNCTIONALITY
                            MAIN:
    When main is called, the above functions are called in the following order:
    1.) sig_handler_constructor - to create the signal handling thread
    2.) signal - to mask the SIGPIPE signal that is sent when client threads terminate
    3.) start_listener - to create the listener thread in which client_constructor is called
                on received client connections
    4.) fgets - to receive input from server terminal until EOF. Depending on the input, 
                client_control_stop, cleint_control_release, or db_print are called.
    5.) sig_handler_destructor - destroys the sig-handler thread in preparation for termination
    6.) delete_all - send a cancellation to each client, prompting them to run thread_cleanup 
                when it is convenient.
    7.) We wait until all threads have terminated after being cancelled using pthread_cond_wait
                to wait for the pthread_broadcast from the last thread to call thread_cleanup.
    8.) db_cleanup - cleanup the database. 
    9.) cancel and join the listener thread. 


KNOWN BUGS:
    I developed on my local version of vagrant. There is a bug on my version of vagrant that I
    posted on piazza about @4282. In addition, signal-handling works such that while cancel
    signals are called to each thread successfully, the server still waits for the clients to
    complete their script before terminating. 