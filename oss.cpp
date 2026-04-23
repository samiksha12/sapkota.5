#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <signal.h>
#include <vector>
#include <sstream>
#include "deadlockdetection.h"

using namespace std;

#define PERMS 0644
#define MAX_RES 10
#define MAX_INST 5
#define MAXIMUM_PROCESS 20

const int BUFF_SZ = sizeof(int) * 2;
const int BILLION = 1000000000;

int available[MAX_RES];
int shm_key;
int shm_id;
int *customClock = nullptr;
int msgqid;

int verboseMode = 1;

// stats
int totalRequests = 0;
int grantedRequests = 0;
int blockedRequests = 0;
int totalReleases = 0;
int deadlockChecks = 0;
int deadlocksDetected = 0;
int processesKilled = 0;
int normalTerminations = 0;

struct PCB
{
    int occupied;
    pid_t pid;
    int startSeconds;
    int startNano;
    int endingTimeSeconds;
    int endingTimeNano;
    int blocked;
    int requestedResource;
    int messageSent;
    int lastResult;
};

PCB processTable[MAXIMUM_PROCESS];
int allocation[MAXIMUM_PROCESS][MAX_RES];

struct msgbuffer
{
    long mtype;
    int value;
    int result;
};

ofstream logfile;

void incrementClock(int running)
{
    if (running == 0)
        return;

    int inc = 250000000 / running;
    customClock[1] += inc;

    while (customClock[1] >= BILLION)
    {
        customClock[0]++;
        customClock[1] -= BILLION;
    }
}

void normalizeTime(int &sec, int &nano)
{
    while (nano >= BILLION)
    {
        sec++;
        nano -= BILLION;
    }
}

bool timeReached(int targetSec, int targetNano)
{
    if (customClock[0] > targetSec)
        return true;
    if (customClock[0] == targetSec && customClock[1] >= targetNano)
        return true;
    return false;
}

void getRandomTime(double t, int &workersec, int &workernano)
{
    int maxSec = (int)t;
    int maxNano = (t - maxSec) * BILLION;

    if (maxSec == 0)
    {
        workersec = 0;
        workernano = (rand() % maxNano) + 1;
        return;
    }

    workersec = (rand() % maxSec) + 1;

    if (workersec == maxSec)
    {
        if (maxNano > 0)
            workernano = rand() % (maxNano + 1);
        else
            workernano = 0;
    }
    else
    {
        workernano = rand() % BILLION;
    }
}

void logmsg(const string &msg)
{
    cout << msg;
    logfile << msg;
    logfile.flush();
}

string timeStamp()
{
    return to_string(customClock[0]) + ":" + to_string(customClock[1]);
}

void initProcessTable()
{
    for (int i = 0; i < MAXIMUM_PROCESS; i++)
    {
        processTable[i].occupied = 0;
        processTable[i].blocked = 0;
        processTable[i].requestedResource = -1;
        processTable[i].lastResult = 0;
        processTable[i].pid = 0;
        processTable[i].messageSent = 0;

        for (int j = 0; j < MAX_RES; j++)
            allocation[i][j] = 0;
    }
}

void printResourceTable()
{
    stringstream out;

    out << "Current system resources at time " << timeStamp() << "\n";
    out << "Total resources available:\n";
    for (int r = 0; r < MAX_RES; r++)
        out << "R" << r << ":" << available[r] << " ";
    out << "\n";

    out << "Resources allocated:\n    ";
    for (int r = 0; r < MAX_RES; r++)
        out << "R" << r << " ";
    out << "\n";

    for (int i = 0; i < MAXIMUM_PROCESS; i++)
    {
        if (processTable[i].occupied)
        {
            out << "P" << i << "  ";
            for (int r = 0; r < MAX_RES; r++)
                out << allocation[i][r] << "  ";
            out << "\n";
        }
    }
    out << "\n";

    logmsg(out.str());
}

bool hasBlockedProcess()
{
    for (int i = 0; i < MAXIMUM_PROCESS; i++)
    {
        if (processTable[i].occupied && processTable[i].blocked)
            return true;
    }
    return false;
}

void buildDeadlockArrays(int req[], int allocFlat[])
{
    for (int i = 0; i < MAXIMUM_PROCESS; i++)
    {
        for (int r = 0; r < MAX_RES; r++)
        {
            allocFlat[i * MAX_RES + r] = allocation[i][r];
            req[i * MAX_RES + r] = 0;
        }

        if (processTable[i].occupied && processTable[i].blocked)
        {
            int rr = processTable[i].requestedResource;
            if (rr >= 0 && rr < MAX_RES)
                req[i * MAX_RES + rr] = 1;
        }
    }
}

vector<int> getDeadlockedSlots()
{
    int req[MAXIMUM_PROCESS * MAX_RES];
    int allocFlat[MAXIMUM_PROCESS * MAX_RES];
    int work[MAX_RES];
    bool finish[MAXIMUM_PROCESS];

    buildDeadlockArrays(req, allocFlat);

    for (int r = 0; r < MAX_RES; r++)
        work[r] = available[r];

    for (int i = 0; i < MAXIMUM_PROCESS; i++)
        finish[i] = false;

    int p = 0;
    for (; p < MAXIMUM_PROCESS; p++)
    {
        if (finish[p])
            continue;

        if (req_lt_avail(req, work, p, MAX_RES))
        {
            finish[p] = true;
            for (int i = 0; i < MAX_RES; i++)
                work[i] += allocFlat[p * MAX_RES + i];
            p = -1;
        }
    }

    vector<int> deadlocked;
    for (int i = 0; i < MAXIMUM_PROCESS; i++)
    {
        if (!finish[i] && processTable[i].occupied)
            deadlocked.push_back(i);
    }

    return deadlocked;
}

bool runDeadlockDetection()
{
    int req[MAXIMUM_PROCESS * MAX_RES];
    int allocFlat[MAXIMUM_PROCESS * MAX_RES];
    buildDeadlockArrays(req, allocFlat);

    deadlockChecks++;
    return deadlock(available, MAX_RES, MAXIMUM_PROCESS, req, allocFlat);
}

void tryUnblockProcesses()
{
    for (int i = 0; i < MAXIMUM_PROCESS; i++)
    {
        if (processTable[i].occupied && processTable[i].blocked)
        {
            int r = processTable[i].requestedResource;

            if (r >= 0 && r < MAX_RES && available[r] > 0)
            {
                available[r]--;
                allocation[i][r]++;
                processTable[i].blocked = 0;
                processTable[i].requestedResource = -1;
                processTable[i].lastResult = r + 1;

                if (verboseMode)
                {
                    logmsg("Master granting P" + to_string(i) + " request R" +
                           to_string(r) + " at time " + timeStamp() + "\n");
                }
            }
        }
    }
}

void recoverFromDeadlock(const vector<int> &deadlockedSlots, int &active)
{
    if (deadlockedSlots.empty())
        return;

    int victim = deadlockedSlots[0];
    pid_t victimPid = processTable[victim].pid;

    stringstream out;
    out << "      Attempting to resolve deadlock...\n";
    out << "      Killing process P" << victim << ": Resources released are as follows: ";

    for (int r = 0; r < MAX_RES; r++)
    {
        if (allocation[victim][r] > 0)
            out << "R" << r << ":" << allocation[victim][r] << " ";
    }
    out << "\n";
    logmsg(out.str());

    kill(victimPid, SIGTERM);
    waitpid(victimPid, NULL, 0);

    for (int r = 0; r < MAX_RES; r++)
    {
        available[r] += allocation[victim][r];
        allocation[victim][r] = 0;
    }

    processTable[victim].occupied = 0;
    processTable[victim].blocked = 0;
    processTable[victim].requestedResource = -1;
    processTable[victim].lastResult = 0;

    processesKilled++;
    active--;
}

void cleanup()
{
    for (int i = 0; i < MAXIMUM_PROCESS; i++)
    {
        if (processTable[i].occupied)
            kill(processTable[i].pid, SIGTERM);
    }

    if (customClock && customClock != (int *)-1)
        shmdt(customClock);

    if (shm_id > 0)
        shmctl(shm_id, IPC_RMID, NULL);

    if (msgqid > 0)
        msgctl(msgqid, IPC_RMID, NULL);

    logfile.close();
}

void signalHandler(int sig)
{
    logmsg("\nOSS: Caught signal. Cleaning up...\n");
    cleanup();
    exit(1);
}

int main(int argc, char *argv[])
{
    int n = -10;
    int s = 2;
    double t = 3.0;
    double interval = 0.2;
    string logname = "logfile.txt";

    int option;
    while ((option = getopt(argc, argv, "hn:s:t:i:f:v:")) != -1)
    {
        switch (option)
        {
        case 'n':
            n = atoi(optarg);
            break;
        case 's':
            s = atoi(optarg);
            break;
        case 't':
            t = atof(optarg);
            break;
        case 'i':
            interval = atof(optarg);
            break;
        case 'f':
            logname = optarg;
            break;
        case 'v':
            verboseMode = atoi(optarg);
            break;
        case 'h':
        default:
            cout << "Usage: oss [-h] [-n proc] [-s simul] [-t iter] [-i interval] [-f logfile] [-v 0|1]\n";
            return 0;
        }
    }

    if (n == -10)
    {
        cerr << "Error: -n is required.\n";
        return 1;
    }

    if (s > n)
        s = n;

    logfile.open(logname);

    shm_key = ftok("test-oss.cpp", 0);
    if (shm_key == -1)
    {
        perror("ftok shm");
        exit(1);
    }

    shm_id = shmget(shm_key, BUFF_SZ, 0700 | IPC_CREAT);
    if (shm_id == -1)
    {
        perror("shmget");
        exit(1);
    }

    customClock = (int *)shmat(shm_id, 0, 0);
    if (customClock == (int *)-1)
    {
        perror("shmat");
        exit(1);
    }

    int *sec = &(customClock[0]);
    int *nano = &(customClock[1]);
    *sec = 0;
    *nano = 0;

    system("touch msgq.txt");
    key_t msgkey = ftok("msgq.txt", 1);
    if (msgkey == -1)
    {
        perror("ftok msg");
        cleanup();
        exit(1);
    }

    msgqid = msgget(msgkey, PERMS | IPC_CREAT);
    if (msgqid == -1)
    {
        perror("msgget");
        cleanup();
        exit(1);
    }

    signal(SIGINT, signalHandler);
    signal(SIGALRM, signalHandler);
    alarm(60);

    initProcessTable();
    for (int i = 0; i < MAX_RES; i++)
        available[i] = MAX_INST;

    srand(time(0));

    int launched = 0;
    int active = 0;
    int currentIndex = 0;
    int totalMessageSent = 0;

    int intervalSec = (int)interval;
    int intervalNano = (interval - intervalSec) * BILLION;

    int lastLaunchSec = 0;
    int lastLaunchNano = 0;

    int workerSec = 0;
    int workerNano = 0;

    pid_t ossPid = getpid();

    static int lastDeadlockCheckSec = -1;

    while (launched < n || active > 0)
    {
        int nextLaunchSec = lastLaunchSec + intervalSec;
        int nextLaunchNano = lastLaunchNano + intervalNano;
        normalizeTime(nextLaunchSec, nextLaunchNano);

        if (launched < n &&
            active < s &&
            (launched == 0 || timeReached(nextLaunchSec, nextLaunchNano)))
        {
            for (int i = 0; i < MAXIMUM_PROCESS; i++)
            {
                if (!processTable[i].occupied)
                {
                    getRandomTime(t, workerSec, workerNano);

                    pid_t worker = fork();
                    if (worker == -1)
                    {
                        perror("fork");
                        cleanup();
                        exit(1);
                    }

                    if (worker == 0)
                    {
                        char secStr[20];
                        char nanoStr[20];
                        char ossPidStr[20];

                        snprintf(secStr, sizeof(secStr), "%d", workerSec);
                        snprintf(nanoStr, sizeof(nanoStr), "%d", workerNano);
                        snprintf(ossPidStr, sizeof(ossPidStr), "%d", ossPid);

                        execl("./worker", "./worker",
                              secStr, nanoStr, ossPidStr, (char *)NULL);

                        perror("execl failed");
                        exit(1);
                    }

                    processTable[i].occupied = 1;
                    processTable[i].pid = worker;
                    processTable[i].startSeconds = *sec;
                    processTable[i].startNano = *nano;
                    processTable[i].endingTimeSeconds = *sec + workerSec;
                    processTable[i].endingTimeNano = *nano + workerNano;
                    normalizeTime(processTable[i].endingTimeSeconds, processTable[i].endingTimeNano);
                    processTable[i].messageSent = 0;
                    processTable[i].blocked = 0;
                    processTable[i].requestedResource = -1;
                    processTable[i].lastResult = 0;

                    active++;
                    launched++;
                    lastLaunchSec = *sec;
                    lastLaunchNano = *nano;
                    break;
                }
            }
        }

        tryUnblockProcesses();

        if (hasBlockedProcess() && *sec != lastDeadlockCheckSec)
        {
            lastDeadlockCheckSec = *sec;

            if (verboseMode)
            {
                logmsg("Master running deadlock detection at time " + timeStamp() + ":\n");
            }

            if (runDeadlockDetection())
            {
                deadlocksDetected++;

                vector<int> deadlockedSlots = getDeadlockedSlots();

                stringstream out;
                out << "      Processes ";
                for (size_t i = 0; i < deadlockedSlots.size(); i++)
                {
                    out << "P" << deadlockedSlots[i];
                    if (i + 1 < deadlockedSlots.size())
                        out << ", ";
                }
                out << " deadlocked\n";
                logmsg(out.str());

                recoverFromDeadlock(deadlockedSlots, active);
                tryUnblockProcesses();
            }
        }

        if (active > 0)
        {
            int checked = 0;
            while (checked < MAXIMUM_PROCESS &&
                   (!processTable[currentIndex].occupied || processTable[currentIndex].blocked))
            {
                currentIndex = (currentIndex + 1) % MAXIMUM_PROCESS;
                checked++;
            }

            if (!processTable[currentIndex].occupied || processTable[currentIndex].blocked)
            {
                incrementClock(active);
                continue;
            }

            pid_t workerPid = processTable[currentIndex].pid;
            int slot = currentIndex;

            msgbuffer sendMsg;
            sendMsg.mtype = workerPid;
            sendMsg.value = 1;
            sendMsg.result = processTable[slot].lastResult;
            processTable[slot].lastResult = 0;

            if (msgsnd(msgqid, &sendMsg, sizeof(msgbuffer) - sizeof(long), 0) == -1)
            {
                perror("msgsnd OSS->worker");
                cleanup();
                exit(1);
            }

            msgbuffer recvMsg;
            if (msgrcv(msgqid, &recvMsg, sizeof(msgbuffer) - sizeof(long), ossPid, 0) == -1)
            {
                perror("msgrcv worker->OSS");
                cleanup();
                exit(1);
            }

            processTable[slot].messageSent++;
            totalMessageSent++;

            if (recvMsg.value > 0)
            {
                int r = recvMsg.value - 1;
                totalRequests++;

                if (verboseMode)
                {
                    logmsg("Master has detected Process P" + to_string(slot) +
                           " requesting R" + to_string(r) +
                           " at time " + timeStamp() + "\n");
                }

                if (r >= 0 && r < MAX_RES && available[r] > 0)
                {
                    available[r]--;
                    allocation[slot][r]++;
                    processTable[slot].lastResult = r + 1;
                    grantedRequests++;

                    if (verboseMode)
                    {
                        logmsg("Master granting P" + to_string(slot) +
                               " request R" + to_string(r) +
                               " at time " + timeStamp() + "\n");
                    }

                    if (grantedRequests % 20 == 0)
                        printResourceTable();
                }
                else
                {
                    processTable[slot].blocked = 1;
                    processTable[slot].requestedResource = r;
                    processTable[slot].lastResult = -(r + 1);
                    blockedRequests++;

                    if (verboseMode)
                    {
                        logmsg("Master cannot grant P" + to_string(slot) +
                               " request for R" + to_string(r) +
                               " at time " + timeStamp() +
                               "; process goes to sleep\n");
                    }
                }
            }
            else if (recvMsg.value < 0)
            {
                int r = (-recvMsg.value) - 1;
                totalReleases++;

                if (r >= 0 && r < MAX_RES && allocation[slot][r] > 0)
                {
                    allocation[slot][r]--;
                    available[r]++;
                }

                if (verboseMode)
                {
                    logmsg("Master has acknowledged Process P" + to_string(slot) +
                           " releasing R" + to_string(r) +
                           " at time " + timeStamp() + "\n");
                }
            }
            else
            {
                if (verboseMode)
                {
                    logmsg("Master has acknowledged Process P" + to_string(slot) +
                           " terminating at time " + timeStamp() + "\n");
                }

                for (int r = 0; r < MAX_RES; r++)
                {
                    available[r] += allocation[slot][r];
                    allocation[slot][r] = 0;
                }

                waitpid(workerPid, NULL, 0);
                processTable[slot].occupied = 0;
                processTable[slot].blocked = 0;
                processTable[slot].requestedResource = -1;
                processTable[slot].lastResult = 0;
                active--;
                normalTerminations++;
            }

            currentIndex = (currentIndex + 1) % MAXIMUM_PROCESS;
        }

        incrementClock(active);
    }

    logmsg("\n===== FINAL STATISTICS =====\n");
    logmsg("Processes launched: " + to_string(launched) + "\n");
    logmsg("Normal terminations: " + to_string(normalTerminations) + "\n");
    logmsg("Requests: " + to_string(totalRequests) + "\n");
    logmsg("Granted: " + to_string(grantedRequests) + "\n");
    logmsg("Blocked: " + to_string(blockedRequests) + "\n");
    logmsg("Releases: " + to_string(totalReleases) + "\n");
    logmsg("Deadlock checks: " + to_string(deadlockChecks) + "\n");
    logmsg("Deadlocks detected: " + to_string(deadlocksDetected) + "\n");
    logmsg("Processes killed: " + to_string(processesKilled) + "\n");
    logmsg("Total messages sent: " + to_string(totalMessageSent) + "\n");

    cleanup();
    return 0;
}