
/*
 * Name: Sarthak Jain
 * Andrew ID: sarthak3
 * tsh - A tiny shell program with job control and I/O redirection.
 * <The line above is not a sufficient documentation.
 *  You will need to write your program documentation.
 *  Follow the 15-213/18-213/15-513 style guide at
 *  http://www.cs.cmu.edu/~213/codeStyle.html.>
 */

#include "tsh_helper.h"
#if 0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include "csapp.h"
#endif


/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

/* Function prototypes */
void eval(const char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);
void bckgrnd_jobs(int output_file_reader);
void runJob_bckgrnd(struct cmdline_tokens token);
void runJob_foregrnd(struct cmdline_tokens token);

/*
 * <Write main's function header documentation. What does main do?>
 * "Each function should be prefaced with a comment describing the purpose
 *  of the function (in a sentence or two), the function's arguments and
 *  return value, any error cases that are relevant to the caller,
 *  any pertinent side effects, and any assumptions that the function makes."
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE_TSH];  // Cmdline for fgets
    bool emit_prompt = true;    // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    Dup2(STDOUT_FILENO, STDERR_FILENO);

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':                   // Prints help message
            usage();
            break;
        case 'v':                   // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p':                   // Disables prompt printing
            emit_prompt = false;
            break;
        default:
            usage();
        }
    }

    // Create environment variable
    if (putenv("MY_ENV=42") < 0) {
        perror("putenv");
        exit(1);
    }


    // Install the signal handlers
    Signal(SIGINT,  sigint_handler);   // Handles ctrl-c
    Signal(SIGTSTP, sigtstp_handler);  // Handles ctrl-z
    Signal(SIGCHLD, sigchld_handler);  // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler);

    // Initialize the job list
    init_job_list();

    // Execute the shell's read/eval loop
    while (true) {
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin)) {
            app_error("fgets error");
        }

        if (feof(stdin)) {
            // End of file (ctrl-d)
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            return 0;
        }

        // Remove the trailing newline
        cmdline[strlen(cmdline)-1] = '\0';

        // Evaluate the command line
        eval(cmdline);

        fflush(stdout);
    }

    return -1; // control never reaches here
}


/* Handy guide for eval:
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg),
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.
 * Note: each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */

/*
 * <What does eval do?>
 */
void eval(const char *cmdline) {
    parseline_return parse_result;
    struct cmdline_tokens token;
	int output_file_flag,input_file_flag;
    // Parse command line
    parse_result = parseline(cmdline, &token);

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY) {
        return;
    }
	if(token.builtin==BUILTIN_QUIT)
		exit(0);
	output_file_flag=open(token.outfile,O_WRONLY|O_CREAT);
	input_file_flag=open(token.outfile,O_RDONLY|O_CREAT);	
	if(token.builtin==BUILTIN_JOBS)
	{
		if(token.outfile!=NULL)
		{
			if(output_file_flag<0)
			{
				printf("Error in opening file");
				return;
			}
			else
				bckgrnd_jobs(output_file_flag);
			close(output_file_flag);
		}
		else
			bckgrnd_jobs(1);		
		return;
	}
	if(token.builtin==BUILTIN_BG)
	{
		runJob_bckgrnd(token);	
		return;
	}
	if(token.builtin==BUILTIN_FG)
	{
		runJob_foregrnd(token);
		return;
	}
    return;
}

/*****************
 * Signal handlers
 *****************/

/*
 * A SIGCHLD Signal is sent to the kernel/parent whenever a child process terminates.
 * The termination can be normally or via receiving some signals such as SIGSTOP,
 * SIGTSTP, SIGTTIN OR SIGTTOU signal.
 * On receiving the SIGCHLD signal, the parent/kernel should reap the newly created
 * zombie process.
 * The parent/kernel would only reap the terminated children and would not wait for the 
 * currently running children to terminate.
 * */
void sigchld_handler(int sig) {
	pid_t term_child_pid;
	int term_child_jid=0,status=0;
	while((term_child_pid=waitpid(-1,&status,WNOHANG|WUNTRACED))>0)
	{
		term_child_jid=find_jid_by_pid(term_child_pid);
		//Case 1: If child terminated normally by itself
		if(WIFEXITED(status))
			delete_job(term_child_pid);
		//Case 2: if child terminated because fo receiving a signal. 
		//We need to print pid,jid and the signal number which stopped the job
		else if(WIFSIGNALED(status))
		{
			printf("Job [%d] (%d) terminated by signal %d \n",term_child_jid,term_child_pid,WTERMSIG(status));
			delete_job(term_child_pid);
		}
		//CASE 3: If child is stopped by the sginal.
		//We need to update the status of the job to ST and not delete from job list
		else if(WIFSTOPPED(status))
		{
			printf("Job [%d] (%d) stopped by signal %d \n",term_child_jid,term_child_pid,WSTOPSIG(status));
			set_state_of_job(find_job_with_pid,ST);
		}
	}
    return;
}

/*
 * Whenever a user has pressed Ctrl+C at the keyboard, it sends a SIGINT Signal.
 * We need to catch that signal and handle it so that it only terminates the foreground running job.
 */
void sigint_handler(int sig) {
	pid_t pid_job_fg=fg_pid();
	if(pid_job_fg==0)
		return;
	if((kill(-pid_job_fg,sig))<0)
		printf("No job running in foregroung \n");
	return;
}

/*
 * Whenever a user has pressed Ctrl+Z at the keyboard, it sends a SIGTSTP signal.
 * We need to catch theat signal and handle it so that it only suspends the foreground running job.
 */
void sigtstp_handler(int sig) {
	pid_t pid_job_fg=fg_pid();
	if(pid_job_fg==0)
		return;
	if((kill(-pid_job_fg,sig))<0)
		printf("No job runnign in foreground \n");
	return;
}


/*
 * This functions helps to list out all bg jobs. Taking help from list_job function in tshhelper.c to remove fg jobs in it.
 * Calling this function to handle builtin command jobs.
 */
void bckgrnd_jobs(int output_file_reader)
{
	check_blocked();
	int i;
	char buf[MAXLINE_TSH];
	for(i=0;i<MAXJOBS;i++)
	{
		memset(buf,'\0',MAXLINE_TSH);
		if(job_list[i].pid!=0)
		{
			if(job_list[i].state==BG)
			{
				sprintf(buf,"[%d] (%d) Running %s\n",job_list[i].jid,job_list[i].pid,job_list[i].cmdline);
				if(write(output_file_reader,buf,strlen(buf))<0){
					fprintf(stderr,"Error writing to output file\n");
					exit(EXIT_FAILURE);
				}
			}
			else if(job_list[i].state==ST)
			{
				sprintf(buf,"[%d] (%d) Running %s\n",job_list[i].jid,job_list[i].pid,job_list[i].cmdline);
				if(write(output_file_reader,buf,strlen(buf))<0){
					fprintf(stderr,"Error writing to output file\n");
					exit(EXIT_FAILURE);
				}
			}
		}
	}
	return;
}

/*This function is for built in bg commang. It runs the appropriate process in bg
 * by sending a SIGCONT signal. 
 *
 * Additionally it also updates the necessary flag of the job.
 *
 */
void runJob_bckgrnd(struct cmdline_tokens token)
{
	int jobid;
	pid_t pid;
	struct job_t *related_job;
	int i=0;
	if(token.argv[1]==NULL){
		printf("No PID or JobId given with command");
		return;
	}
	if(token.argv[1][0]=='%')//given argv[1] is JOB ID as it has leading % sign
	{
		for(i=1;i<strlen(token.argv[1]);i++)//start from index 1 to ignore % sign
		{
			if(!isDigit(token.argv[1][i]))
			{
				printf("All digits of Job id and Pid must be numeric");
				return;
			}
		}
		jobid=atoi(token.argv[1]+sizeof(char));
		related_job=find_job_with_jid(jobid);
		if(related_job==NULL)
		{
			printf("No job with this JOB id \n");
			return;
		}
		if(kill(-(related_job->pid),SIGCONT)<0){
			printf("Unable to start the given job in bg \n");
			return;
		}
		printf("[%d] (%d) %s\n",related_job->jid,related_job->pid,related_job->cmdline);
		related_job->state=BG;
	}
	else//given is the PID so start checking argv[1] from 0
	{
		for(i=0;i<strlen(token.argv[1]);i++)
		{
			if(!isDigit(token.argv[1][i]))
			{
				printf("All digits of Job id and Pid must be numeric");
				return;
			}
		}
		pid=atoi(token.argv[1]+sizeof(char));
		related_job=find_job_with_pid(pid);
		if(related_job==NULL)
		{
			printf("No job with this  Pid \n");
			return;
		}
		if(kill(-(related_job->pid),SIGCONT)<0){
			printf("Unable to start the given job in bg \n");
			return;
		}
		printf("[%d] (%d) %s\n",related_job->jid,related_job->pid,related_job->cmdline);
		related_job->state=BG;	
	}
	return;
}

/*
 *This function is for the builtin command fg. It takes Job or PID as input
 *and start the stop by sending SIGCONT signal.
 *
 * It also updates the necessary members of the given job such as state
 */
void runJob_foregrnd(struct cmdline_tokens token)
{
	int jobid;
	pid_t pid;
	struct job_t *related_job;
	int i=0;
	sigset_t mask;
	if(token.argv[1]==NULL){
		printf("No PID or JobId given with command");
		return;
	}
	if(token.argv[1][0]=='%')//given argv[1] is JOB ID as it has leading % sign
	{
		for(i=1;i<strlen(token.argv[1]);i++)//start from index 1 to ignore % sign
		{
			if(!isDigit(token.argv[1][i]))
			{
				printf("All digits of Job id and Pid must be numeric");
				return;
			}
		}
		jobid=atoi(token.argv[1]+sizeof(char));
		related_job=find_job_with_jid(jobid);
		if(related_job==NULL)
		{
			printf("No job with this JOB id \n");
			return;
		}
		if(kill(-(related_job->pid),SIGCONT)<0){
			printf("Unable to start the given job in fg \n");
			return;
		}
		related_job->state=FG;
		sigemptyset(&mask);
		while(fg_pid()!=0 && related_job->state==FG)
			sigsuspend(&mask);
	}
	else//given is the PID so start checking argv[1] from 0
	{
		for(i=0;i<strlen(token.argv[1]);i++)
		{
			if(!isDigit(token.argv[1][i]))
			{
				printf("All digits of Job id and Pid must be numeric");
				return;
			}
		}
		pid=atoi(token.argv[1]+sizeof(char));
		related_job=find_job_with_pid(pid);
		if(related_job==NULL)
		{
			printf("No job with this PID id \n");
			return;
		}
		if(kill(-(related_job->pid),SIGCONT)<0){
			printf("Unable to start the given job in fg \n");
			return;
		}
		related_job->state=FG;
		sigemptyset(&mask);
		while(fg_pid!=0 && related_job->state==FG)
			sigsuspend(&mask);
	}
	return;
}
