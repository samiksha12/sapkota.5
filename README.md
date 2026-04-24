Name: Samiksha Sapkota
Date: April 15, 2026
Environment: Mac , Visual Studio Code
How to compile the project:
Type 'make' ->Enter
Type 'touch msgq.txt' (it is working without this too, but just to be on safe side)->Enter

Example of how to run the project:
./oss -n 5 -s 5 -t 5 -i 0.2

Program behaviour:

 I've used project 3 as a base for this project. I've worked step by step as your suggested implementation steps. In this project, oss is creating resource descriptors and populating them with instances. We have 10 resources classes, with 5 instances of each resource class. Oss maintain resource that have a detail of available resources. Worker also maintain their own resource table where they store the detail of resources available to them. OSS and worker comunicate by sending messages. Oss create a child worker and send the termination time to worker. Worker request, release 1 resource at a time, if it is available OSS grants them to worker.
 Worker is randomly deciding 70% of time to request a resource and 30% of time to release a resource, but the decision is completely random. When oss don't have the resource to grant it is blocking the worker and not sending any further message till the resource is available, and once it is available it will grant the requesting worker. Worker is sending a number, this number maps to our resource class number. If the number is positive Oss will interpret it as worker is requesting for that number resource, if it is negative it indicates the worker is releasing the resource. When its time to terminate worker sends 0 to Oss and Oss releases all the resource allocated to that worker pid. I've used your deadlock detection code, and running it every second. Oss has a request and allocation array which I've passed it to your deadlock() function. It returns true, meaning there is a deadlock. And once the deadlock is detected, oss will chose one process as victim from those process and kill it. Once the process is killed it will release the resource and grant the resource if it is requested by any other process, by unblocking them first.  

 Generative AI used: ChatGPT and Google AI overview:

 prompt: what is verbose and how to use it (Google AI overview)
 Suggestion: In programming, verbose refers to an optional "wordy" mode where a program provides detailed, step-by-step information about its internal operations.
 I used it as a boolean variable to print the log output.

 prompt: Help me solve the zsh: segmentation fault ./oss -n 2 -s 2 -t 2 -i 0.5 -f log.txt (Chat gpt)
 Suggestion: to save getpid() in a variable and add variable to our msgsnd(). I used the working project 3 but for some reason I was getting the segmentation fault error. I tried to debug, oss was send the message, worker was receiving it, worker sent the message and oss never received any message. once I changed the getpid() with variable it worked.

prompt: Help me with deadlock detection and recovery.
Summary: I completed the step 7, and succesfully detected the deadlock, but for recovering from the deadlock and unblocking the process, I got stuck with infinite loops so I asked for the chatgpt help and it suggested me creating few functions that gave me clear idea of how to move ahead 
