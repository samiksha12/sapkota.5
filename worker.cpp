#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <cstdlib>
#include <ctime>

using namespace std;

#define PERMS 0644
#define MAX_RES 10
#define MAX_INST 5

const int BILLION = 1000000000;
const int BUFF_SZ = sizeof(int) * 2;

struct msgbuffer
{
    long mtype;
    int value;
    int result;
};

bool timeUp(int secNow, int nanoNow, int termSec, int termNano)
{
    if (secNow > termSec)
        return true;
    if (secNow == termSec && nanoNow >= termNano)
        return true;
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        cout << "Usage: ./test-worker sec nano ossPid\n";
        return 1;
    }

    int seconds = atoi(argv[1]);
    int nanoseconds = atoi(argv[2]);
    int ossPid = atoi(argv[3]);

    srand(getpid());

    int shm_key = ftok("oss.cpp", 0);
    if (shm_key == -1)
    {
        perror("ftok shm");
        exit(1);
    }

    int shm_id = shmget(shm_key, BUFF_SZ, 0700);
    if (shm_id == -1)
    {
        perror("shmget");
        exit(1);
    }

    int *clockptr = (int *)shmat(shm_id, 0, 0);
    if (clockptr == (int *)-1)
    {
        perror("shmat");
        exit(1);
    }

    int *sec = &(clockptr[0]);
    int *nano = &(clockptr[1]);

    int termTimeSec = *sec + seconds;
    int termTimeNano = *nano + nanoseconds;
    if (termTimeNano >= BILLION)
    {
        termTimeSec++;
        termTimeNano -= BILLION;
    }

    key_t key = ftok("msgq.txt", 1);
    if (key == -1)
    {
        perror("ftok msg");
        exit(1);
    }

    int msgqid = msgget(key, PERMS);
    if (msgqid == -1)
    {
        perror("msgget");
        exit(1);
    }

    int myResources[MAX_RES] = {0};

    while (true)
    {
        msgbuffer msg;

        if (msgrcv(msgqid, &msg, sizeof(msgbuffer) - sizeof(long), getpid(), 0) == -1)
        {
            perror("receive fail");
            shmdt(clockptr);
            exit(1);
        }

        // Apply feedback from OSS about the PREVIOUS request
        if (msg.result > 0)
        {
            int r = msg.result - 1;
            if (r >= 0 && r < MAX_RES && myResources[r] < MAX_INST)
                myResources[r]++;
        }

        msg.mtype = ossPid;
        msg.result = 0;

        if (timeUp(*sec, *nano, termTimeSec, termTimeNano))
        {
            msg.value = 0;
            if (msgsnd(msgqid, &msg, sizeof(msgbuffer) - sizeof(long), 0) == -1)
            {
                perror("send terminate fail");
                shmdt(clockptr);
                exit(1);
            }
            break;
        }

        int action = rand() % 100;

        if (action < 70)
        {
            // request
            int choices[MAX_RES];
            int count = 0;

            for (int i = 0; i < MAX_RES; i++)
            {
                if (myResources[i] < MAX_INST)
                {
                    choices[count] = i;
                    count++;
                }
            }

            if (count > 0)
            {
                int r = choices[rand() % count];
                msg.value = r + 1;
            }
            else
            {
                // fallback release
                int releasable[MAX_RES];
                int relCount = 0;
                for (int i = 0; i < MAX_RES; i++)
                {
                    if (myResources[i] > 0)
                    {
                        releasable[relCount] = i;
                        relCount++;
                    }
                }

                if (relCount > 0)
                {
                    int r = releasable[rand() % relCount];
                    msg.value = -(r + 1);
                    myResources[r]--;
                }
                else
                {
                    msg.value = 1;
                }
            }
        }
        else
        {
            // release
            int releasable[MAX_RES];
            int count = 0;

            for (int i = 0; i < MAX_RES; i++)
            {
                if (myResources[i] > 0)
                {
                    releasable[count] = i;
                    count++;
                }
            }

            if (count > 0)
            {
                int r = releasable[rand() % count];
                msg.value = -(r + 1);
                myResources[r]--;
            }
            else
            {
                int choices[MAX_RES];
                int reqCount = 0;
                for (int i = 0; i < MAX_RES; i++)
                {
                    if (myResources[i] < MAX_INST)
                    {
                        choices[reqCount] = i;
                        reqCount++;
                    }
                }

                if (reqCount > 0)
                {
                    int r = choices[rand() % reqCount];
                    msg.value = r + 1;
                }
                else
                {
                    msg.value = 1;
                }
            }
        }

        if (msgsnd(msgqid, &msg, sizeof(msgbuffer) - sizeof(long), 0) == -1)
        {
            perror("send action fail");
            shmdt(clockptr);
            exit(1);
        }
    }

    shmdt(clockptr);
    return 0;
}