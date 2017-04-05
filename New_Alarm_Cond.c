/*
 * alarm_cond.c
 *
 * This is an enhancement to the alarm_mutex.c program, which
 * used only a mutex to synchronize access to the shared alarm
 * list. This version adds a condition variable. The alarm
 * thread waits on this condition variable, with a timeout that
 * corresponds to the earliest timer request. If the main thread
 * enters an earlier timeout, it signals the condition variable
 * so that the alarm thread will wake up and process the earlier
 * timeout first, requeueing the later request.
 */
#include <pthread.h>
#include <time.h>
#include <semaphore.h>
#include "errors.h"
#include <stdio.h>

#define TYPE_A 0
#define TYPE_B 1

#define FIRST_ALARM 0
#define REPLACEMENT 1
#define NO_MATCHING_ALARM 2
#define MULTIPLE_CANCEL 3
#define CANCEL_REQ 4


/*
 * The "alarm" structure now contains the time_t (time since the
 * Epoch, in seconds) for each alarm, so that they can be
 * sorted. Storing the requested number of seconds would not be
 * enough, since the "alarm thread" cannot tell how long it has
 * been on the list.
 */
typedef struct alarm_tag {
    struct alarm_tag    *link;
    int                 seconds;
    int                 alarm_number;
    int                 request_type;
    time_t              time;   /* seconds from EPOCH */
    char                message[128];
    int                 changed;
} alarm_t;

typedef struct append_list{
    alarm_t *           alarm;
    struct append_list* next;
    struct append_list* last;
} append_list;

typedef struct removed_list {
    int             removed_num;
    struct removed_list *  next;
} removed_list;

typedef struct thread_specific_alarm {
    alarm_t *       alarm;
    int             removed;
} thread_alarm;

typedef struct thread_alarm_list {
    struct thread_alarm_list *   next;
    struct thread_alarm_list *   previous;
    thread_alarm *               data;
} display_thread_list;

removed_list * removed = NULL;

int alarm_thread_flag = 0;
int reader_flag = 0;
//for alarm list cleanup

alarm_t *alarm_list = NULL;
append_list *list_to_append = NULL;

sem_t alarm_semaphore;
sem_t display_sem;

int append_flag = 0;
int delete_flag = 0;

display_thread_list * thread_list = NULL;

/*
 * Finds a thread in the list to mark as removed.
 *
 */
void find_in_list(int alarm_num){
    display_thread_list * list = thread_list;
    display_thread_list * old;
    while(list != NULL){
        if(list->data->alarm->alarm_number == alarm_num){
            //Mark as removed;
            list->data->removed = 1;
            //Remove from list, the thread will clean itself up
            if(list->next != NULL)
                list->next->previous;
            if(list->previous != NULL)
                list->previous->next = list->next;

            if(thread_list == list){
             *(&thread_list) = list->next;
            }
            old = list;
            free(old);
            return;
        }
        list = list->next;
    }
}



/*
 * Function to execute a cancel request.
 * In the case of multiple type B request, it will remove all type B requests inserted in one go.
 * Note: last_rem should be null
 *
 */
void alarm_delete(){
    alarm_t **last, *next, *to_remove;

    /*Locking protocol:
     *
     * We will assume prior to calling this method, it will wait for the writer semaphore.
     *
     */
    last = &alarm_list;
    next = *last;
    while(next != NULL){
        if(next->request_type == TYPE_B){
            to_remove = next->link;
            //Check if to_remove is of type A
            if(to_remove != NULL && to_remove->request_type == TYPE_A){
                //Before removing, mark thread as removed
                find_in_list(to_remove->alarm_number);

                //Remove both the cancellation requests as well as alarm itself
                *last = to_remove->link;
                free(to_remove);
                free(next);
                next = *last;
                continue;
            }
        }
        last = &next->link;
        next = next->link;
    }
};

/*
 * Insert alarm entry on list, in order.
 * Note: It will always insert a type B request in front of a type A, as is requred by
 * alarm_delete.
 */
int alarm_insert (alarm_t *alarm)
{
    int status;
    alarm_t **last, *next;

    /*
     * LOCKING PROTOCOL:
     * 
     * This routine requires that the caller have locked the
     * alarm_mutex!
     */
    last = &alarm_list;
    next = *last;
    while (next != NULL) {
        if (next->alarm_number >= alarm->alarm_number) {
            //If both alarm types are the same, handle is based on the message type
            if(next->alarm_number == alarm->alarm_number){
                //If alarms are of the same type
                if(next->request_type == alarm->request_type) {
                    //If it is a replacement,
                    if(alarm->request_type == TYPE_A){
                        //Copy the message and the new time
                        next->seconds = alarm->seconds;
                        next->time = alarm->time;
                        strcpy(next->message, alarm->message);
                        next->changed = 1;
                        //Free the previous alarm
                        free(alarm);
                        //Replace its pointer in memory.

                        //Return a replacement code
                        return REPLACEMENT;
                    }
                    //Otherwise, both are type B free the alarm, signal multiple cancel.
                    free(alarm);
                    return MULTIPLE_CANCEL;
                }
                //Otherwise, they are of different types.
                //If alarm is of type B and next is of type A, append alarm in front of next.
                if(alarm->request_type == TYPE_B){
                    alarm->link = next;
                    *last = alarm;
                    return CANCEL_REQ;
                }

            }
            //If its' a standalone B that doesn't match, return error and free
            //Leave list unchanged.
            if(alarm->request_type == TYPE_B){
                free(alarm);
                return NO_MATCHING_ALARM;
            }

            alarm->link = next;
            *last = alarm;
            return FIRST_ALARM;
        }
        last = &next->link;
        next = next->link;
    }
    /*
     * If we reached the end of the list, insert the new alarm
     * there.  ("next" is NULL, and "last" points to the link
     * field of the last item, or to the list header.)
     */
    if (next == NULL) {

        if(alarm->request_type == TYPE_A){
            *last = alarm;
            alarm->link = NULL;
            return FIRST_ALARM;
        }
        free(alarm);
        return NO_MATCHING_ALARM;
    }
}

/*
 * The display thread
 * arg gets cast into the thread_alarm struct containing information
 * pertaining to the specific thread
 *
 */
void * display_thread(void * arg) {

    thread_alarm * alarm = (thread_alarm *) arg;

    char msg[128];
    strcpy(msg, alarm->alarm->message);
    int alarm_num = alarm->alarm->alarm_number;
    int interval = alarm->alarm->seconds;
    int has_changed =0;
    time_t now;
    time_t display_interval = time(NULL);

    while(1){
        sem_wait(&display_sem);
        reader_flag++;
        if(reader_flag == 1)
            sem_wait(&alarm_semaphore);
        sem_post(&display_sem);
        now = time(NULL);
        //If alarm was removed, exit.
        if(alarm->removed == 1){
            printf("Display thread exiting at time %d: %d Message(%d) %s\n",
                   (int) now, interval, alarm_num, msg);
            sem_wait(&display_sem);
            reader_flag--;
            if(reader_flag ==0)
                sem_post(&alarm_semaphore);
            sem_post(&display_sem);
            pthread_exit(NULL);
        }

        if(alarm->alarm->changed == 1){

            printf("Alarm With Message Number (%d) Replaced at %d: %d Message(%d) %s\n",
                   alarm->alarm->alarm_number, (int) now, alarm->alarm->seconds,alarm->alarm->alarm_number, alarm->alarm->message);
            interval = alarm->alarm->seconds;
            display_interval = now + interval;
            has_changed = 1;
            alarm->alarm->changed = 0;
            strcpy(msg, alarm->alarm->message);
        } else if(now >= display_interval){
            if(has_changed == 1){
                printf("Replacement Alarm With Message Number (%d) Displayed at %d: %d Message(%d) %s\n",
                       alarm->alarm->alarm_number, (int) now, alarm->alarm->seconds,alarm->alarm->alarm_number, alarm->alarm->message);

            } else {
                printf("Alarm With Message Number (%d) Displayed at %d: %d Message(%d) %s\n",
                       alarm->alarm->alarm_number, (int) now, alarm->alarm->seconds,alarm->alarm->alarm_number, alarm->alarm->message);
            }
            display_interval = now+ alarm->alarm->seconds;
        }

        sem_wait(&display_sem);
        reader_flag--;
        if(reader_flag ==0)
            sem_post(&alarm_semaphore);
        sem_post(&display_sem);
    }

}

/*
 * Creates the list of display threads based on the last appended to the list.
 *
 */
display_thread_list * create_display_threads(display_thread_list * last){
    append_list * old;
    display_thread_list * new_list_node;
    thread_alarm * new_thread_alarm;
    int counter = 0;
    while(list_to_append != NULL){
        pthread_t new_thread;
        old = list_to_append;
        new_list_node = (display_thread_list *)malloc(sizeof(display_thread_list));
        new_list_node->previous = last;
        new_list_node->next = NULL;
        //Initialize the new thread's data
        new_thread_alarm = (thread_alarm *) malloc(sizeof(thread_alarm));
        new_thread_alarm->alarm = old->alarm;
        new_thread_alarm->removed = 0;
        //Add the new thread data to a list struct
        last->data = new_thread_alarm;
        //Append the
        last->next = new_list_node;
        last = last->next;

        list_to_append = list_to_append->next;
        pthread_create(&new_thread, NULL, display_thread, (void *) new_thread_alarm);
        free(old);
        counter++;
    }
    flockfile(stdout);
    printf("Created a bunch of shit: %d\n", counter);
    fflush(stdout);
    funlockfile(stdout);

    //Return the reference to the last element
    return last;

}

/*
 * The alarm thread's start routine.
 */
void *alarm_thread (void *arg)
{
    int status;
    removed = (removed_list *)malloc(sizeof(removed_list));
    thread_list = (display_thread_list *) malloc(sizeof(display_thread_list));
    display_thread_list * last = thread_list;

    /*
     * Loop forever, processing commands. The alarm thread will
     * be disintegrated when the process exits. Lock the mutex
     * at the start -- it will be unlocked during condition
     * waits, so the main thread can insert alarms.
     */
    while (1) {

        //Busy wait while the flag is 0
        while(alarm_thread_flag == 0);
        sem_wait(&alarm_semaphore);

        if(append_flag == 1) {

            last = create_display_threads(thread_list);
            append_flag = 0;

        } else if (delete_flag == 1){
            alarm_delete();
            delete_flag = 0;
        }

        alarm_thread_flag = 0;
        sem_post(&alarm_semaphore);

    }
}

int main (int argc, char *argv[])
{
    int status;
    char line[160]; //Messages with higher allocated size
    char msg[10];
    char cancellation[10];
    int err_num, message_num;
    time_t now;
    alarm_t *alarm;
    pthread_t thread;
    append_list * to_append;

    if(sem_init(&alarm_semaphore,0,1) < 0){
        printf("Error creating semaphore!");
        exit(1);
    }

    if(sem_init(&display_sem,0,1) < 0){
        printf("Error creating semaphore!");
        exit(1);
    }



    status = pthread_create (
            &thread, NULL, alarm_thread, NULL);
    if (status != 0)
        err_abort (status, "Create alarm thread");
    while (1) {

        printf ("Alarm> ");
        if (fgets (line, sizeof (line), stdin) == NULL) exit (0);
        if (strlen (line) <= 1) continue;
        alarm = (alarm_t*)malloc (sizeof (alarm_t));
        if (alarm == NULL)
            errno_abort ("Allocate alarm");

        //Parse input if it's of type A, check format correctness
        if (sscanf (line, "%d %10[^(](%d) %128[^\n]", &alarm->seconds, msg, &alarm->alarm_number,alarm->message) == 4) {
            //Check if message is in the right format
            if (strcmp(msg, "Message") == 0) {
                //This is a type A message
                now = time(NULL);
                alarm->link = NULL;
                alarm->request_type = TYPE_A;
                alarm->time = now + alarm->seconds;
                alarm->changed = 0;
                //Main thread always counts as a writer, never a reader.
                status = sem_wait(&alarm_semaphore);
                if (status != 0)
                    err_abort (status, "Lock mutex");

                err_num = alarm_insert(alarm);
                //Check the return type of the function.
                switch (err_num) {
                    case FIRST_ALARM:
                        printf("First Alarm Request With Message Number (%d) Received at %d: %d Message(%d) %s\n",
                               alarm->alarm_number, (int) now, alarm->seconds, alarm->alarm_number, alarm->message);
                        to_append = (append_list *) malloc(sizeof(append_list));
                        to_append->next = NULL;
                        to_append->alarm = alarm;
                        to_append->last = NULL;
                        if(list_to_append == NULL) {
                            list_to_append = to_append;
                        } else {
                            if(list_to_append->next == NULL){
                                list_to_append->next = to_append;
                                list_to_append->last = to_append;
                            } else {
                                list_to_append->last->next = to_append;
                                list_to_append->last = to_append;
                            }
                        }
                        append_flag = 1;



                        break;
                    case REPLACEMENT:
                        printf("Replacement Alarm Request With Message Number (%d) Received at %d: %d Message(%d) %s\n",
                               alarm->alarm_number, (int) now, alarm->seconds, alarm->alarm_number, alarm->message);
                        break;
                    default:
                        err_abort(1, "Alarm added an incorrect type");
                        break;
                }
                alarm_thread_flag = 1;

                status = sem_post(&alarm_semaphore);
                if (status != 0)
                    err_abort (status, "Unlock mutex");


            } else {
                printf("Error: Incorrect format\n");

            }
            //Else, check if of type b
        } else if(sscanf (line, "%[^:]: %10[^(](%d)", cancellation, msg, &message_num) == 3){
            if(strcmp(cancellation, "Cancel") == 0) {
                //Alarm is of type b
                now = time(NULL);
                alarm->alarm_number = message_num;
                alarm->request_type = TYPE_B;

                status = sem_wait(&alarm_semaphore);
                if (status != 0)
                    err_abort (status, "Lock mutex");

                err_num = alarm_insert(alarm);
                //Check the return type of the function.
                switch(err_num){
                    case NO_MATCHING_ALARM:
                        printf("Error: No Alarm Request With Message Number (%d) to Cancel!\n",
                               message_num);
                        break;
                    case MULTIPLE_CANCEL:
                        printf("Error: More Than One Request to Cancel Alarm Request With Message Number (%d)\n",
                               message_num);
                        break;
                    case CANCEL_REQ:
                        printf("Cancel Alarm Request With Message Number (Message_Number) Received at %d: Cancel: Message(%d)\n",
                               (int) now, alarm->alarm_number);
                        alarm_thread_flag = 1;
                        delete_flag = 1;
                        break;
                    default:
                        err_abort(1, "Alarm added an incorrect type");
                }

                status = sem_post(&alarm_semaphore);
                if (status != 0)
                    err_abort (status, "Unlock mutex");


            } else {
                printf("Error: Incorrect format\n");
            }
        } else {
            fprintf (stderr, "Bad command\n");
            free (alarm);
        }
    }
}
