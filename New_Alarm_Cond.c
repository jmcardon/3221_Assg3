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

typedef struct last_changed_alarm {
    alarm_t *           alarm;
    int                 last_status;
    int                 changed;
} last_changed;

typedef struct last_removed {
    int             removed_num;
} last_removed;

last_removed * removed;

int alarm_thread_flag = 0;
int reader_flag = 0;
//for alarm list cleanup

alarm_t *alarm_list = NULL;

sem_t alarm_semaphore;
sem_t display_sem;

/*
 * Function to execute a cancel request.
 * In the case of multiple type B request, it will remove all type B requests inserted in one go.
 *
 */
void alarm_delete(last_removed * last_rem){
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
                //If it is, it's been inserted correctly, now remove both.
                last_rem->removed_num = to_remove->alarm_number;
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
                        //Copy the mssage and the new time
                        next->seconds = alarm->seconds;
                        next->time = alarm->time;
                        strcpy(next->message, alarm->message);
                        //Free the previous alarm
                        free(alarm);
                        //Replace its pointer in memory.
//                        *last = alarm;
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
#ifdef DEBUG
    printf ("[list: ");
    for (next = alarm_list; next != NULL; next = next->link)
        printf ("%d(%d)[\"%s\"] ", next->time,
            next->time - time (NULL), next->message);
    printf ("]\n");
#endif

}


void * display_thread(void * arg) {

    alarm_t * thread_alarm = (alarm_t *) arg;
    char msg[128];
    strcpy(msg, thread_alarm->message);
    int alarm_num = thread_alarm->alarm_number;
    int interval = thread_alarm->seconds;
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
        if(removed->removed_num == alarm_num){
            removed->removed_num = -1;
            printf("Display thread exiting at time %d: %d Message(%d) %s\n",
                   (int) now, interval, alarm_num, msg);
            sem_wait(&display_sem);
            reader_flag--;
            if(reader_flag ==0)
                sem_post(&alarm_semaphore);
            sem_post(&display_sem);
            pthread_exit(NULL);
        }

        if(strcmp(msg, thread_alarm->message) != 0 || thread_alarm->seconds != interval){

            printf("Alarm With Message Number (%d) Replaced at %d: %d Message(%d) %s\n",
                   thread_alarm->alarm_number, (int) now, thread_alarm->seconds,thread_alarm->alarm_number, thread_alarm->message);
            interval = thread_alarm->seconds;
            thread_alarm->changed = 1;
            display_interval = now + interval;
            strcpy(msg, thread_alarm->message);
        }

        //Perform read
        if(now >= display_interval){
            if(thread_alarm->changed == 1){
                printf("Replacement Alarm With Message Number (%d) Displayed at %d: %d Message(%d) %s\n",
                       thread_alarm->alarm_number, (int) now, thread_alarm->seconds,thread_alarm->alarm_number, thread_alarm->message);

            } else {
                printf("Alarm With Message Number (%d) Displayed at %d: %d Message(%d) %s\n",
                       thread_alarm->alarm_number, (int) now, thread_alarm->seconds,thread_alarm->alarm_number, thread_alarm->message);
            }
            display_interval = now+ thread_alarm->seconds;
        }

        sem_wait(&display_sem);
        reader_flag--;
        if(reader_flag ==0)
            sem_post(&alarm_semaphore);
        sem_post(&display_sem);
    }

}

/*
 * The alarm thread's start routine.
 */
void *alarm_thread (void *arg)
{
    alarm_t *alarm;
    int status;
    last_changed * last= (last_changed *)arg;
    removed = (last_removed *)malloc(sizeof(last_removed));

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

        if(last->changed == 1) {

            if (last->last_status == FIRST_ALARM) {
                //Create new thread
                pthread_t new_thread;
                status = pthread_create(&new_thread, NULL, display_thread, (void *) last->alarm);
                if (status != 0)
                    err_abort (status, "Create display thread");

            } else if (last->last_status == CANCEL_REQ) {
                //Cancel request
                alarm_delete(removed);
            }
            //Set flag to 0

            last->changed = 0;
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
    last_changed * last = (last_changed *)malloc(sizeof(last_changed));

    if(sem_init(&alarm_semaphore,0,1) < 0){
        printf("Error creating semaphore!");
        exit(1);
    }

    if(sem_init(&display_sem,0,1) < 0){
        printf("Error creating semaphore!");
        exit(1);
    }



    status = pthread_create (
            &thread, NULL, alarm_thread, (void *)last);
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
                        last->alarm = alarm;
                        last->last_status = FIRST_ALARM;
                        last->changed = 1;
                        printf("First Alarm Request With Message Number (%d) Received at %d: %d Message(%d) %s\n",
                               alarm->alarm_number, (int) now, alarm->seconds, alarm->alarm_number, alarm->message);
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
                        last->last_status = CANCEL_REQ;
                        last->alarm = alarm;
                        last->changed = 1;
                        alarm_thread_flag = 1;
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
