/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/*
 * Minish shell.
 *
 * Add The name of the first team member here in the form Surname Lastname, iis...
 * Add The name of the first team member here in the form Surname Lastname, iis...
*/

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "parser.h"

#define INPUT_SIZE 4096
#define MAX_JOBS 128

struct job {
	pid_t pid;
	int active;
};

// This array can hold information about spawned processes.
// Each array element describes a new process and holds its
// PID and its state in the active variable. This variable
// may take the value 1 if the job is active, or the value
// 0 if the job is not active. The array can be used to
// manage background jobs.
//
// You may use this array if you find it helpful. Otherwise,
// fill free to delete it.
static struct job jobs[MAX_JOBS];

static void print_prompt(void);
static int read_line(char *buffer, size_t size);
static int execute_pipeline(const struct pipeline *pipeline);
static void init_bg_jobs(struct job *jobs);
static void add_bg_job(const pid_t pid);
static void remove_bg_job(const pid_t pid);
static void reap_bg_jobs(void);
static void handle_sigchld(int sig);

// Add the prototypes of helper funcitons here...

/**
 * @brief Run the minish command interpreter.
 *
 * @return Process exit status.
 */
int main(void)
{	
	char line[INPUT_SIZE];
	struct pipeline pipeline;
	int error;
	int result;

	init_bg_jobs(jobs);

	// Install the required signal handlers here.
	struct sigaction chld_action;
	memset(&chld_action, 0, sizeof(chld_action));
	chld_action.sa_handler = handle_sigchld;
	sigemptyset(&chld_action.sa_mask);
	chld_action.sa_flags = SA_RESTART;

	if (sigaction(SIGCHLD, &chld_action, NULL) < 0) perror("sigaction SIGCHLD");

	struct sigaction int_action;
	memset(&int_action, 0, sizeof(int_action));
	int_action.sa_handler = SIG_IGN;
	sigemptyset(&int_action.sa_mask);

	if (sigaction(SIGINT, &int_action, NULL) < 0) {
		perror("sigaction SIGINT");
		return EXIT_FAILURE;
	}
	 
	// The main shell loop.
	for (;;) {
		// Print the prompt.
		print_prompt();

		// Read a command line from the user.
		result = read_line(line, sizeof(line));

		// Handle read errors.
		if (result == 0)
			break;

		if (result < 0) {
			if (errno == EINTR)
				continue;

			perror("minish: Cannot read the command line.");
			return EXIT_FAILURE;
		}

		// Parse the line.
		error = parse_line(line, &pipeline);

		// An error occurred during parsing.
		if (error != PARSE_SUCCESS) {
			fprintf(stderr, "minish: %s\n", parse_error_string(error));
			continue;
		}

		// The user just pressed enter. We have nothing to execute at this time.
		if (pipeline.command_count == 0) {
			pipeline_free(&pipeline);
			continue;
		}

		// Execute the command pipeline.
		if ((result = execute_pipeline(&pipeline)) < 0)
			perror("minish");

		// Clear the pipeline.
		pipeline_free(&pipeline);
	}
	return EXIT_SUCCESS;
}

static void handle_sigchld(int sig) {
	(void)sig;
	reap_bg_jobs();
}

/**
 * @brief Print the interactive shell prompt.
 */
static void print_prompt(void)
{
	if (isatty(STDIN_FILENO) != 0) {
		char cwd[INPUT_SIZE];

		if (getcwd(cwd, sizeof(cwd)) != NULL) (void)printf("minish:%s$ ", cwd);
		else (void)printf("minish$ ");
		
		(void)fflush(stdout);
	}
}

/**
 * @brief Read one command line from standard input.
 *
 * @param[out] buffer Destination buffer.
 * @param[in] size Size of the destination buffer.
 *
 * @return 1 when a line was read, 0 on EOF, or -1 on error.
 */
static int read_line(char *buffer, size_t size)
{
	if (fgets(buffer, size, stdin) == NULL) {
		if (feof(stdin) != 0)
			return 0;

		return -1;
	}

	buffer[strcspn(buffer, "\n")] = '\0';

	return 1;
}

void init_bg_jobs(struct job *jobs)
{
	int i;
  
	if (!jobs)
		return;
		
	for (i = 0; i < MAX_JOBS; i++) {		
		jobs[i].pid =-1;
		jobs[i].active = 0;
	}
}

void add_bg_job(const pid_t pid)
{
	int i;

	for (i = 0; i < MAX_JOBS; i++) {
		// Find the first position in the array with active == 0 and add the
		// new job there. Then call break to exit the loop.
        if (jobs[i].active == 0) {
			jobs[i].pid = pid;
			jobs[i].active = 1;
			break;
		}
	}
}

void remove_bg_job(const pid_t pid)
{
	int i;

	for (i = 0; i < MAX_JOBS; i++) {
		// Find the first position in the array where its pid matches with the
		// pid parameter of the function and then clean it by setting pid to
		// -1 and active to 0.
        if (jobs[i].pid == pid && jobs[i].active == 1) {
			jobs[i].pid = -1;
			jobs[i].active = 0;
			break;
		}
	}
}

// The function below should be called from a signal handler.
void reap_bg_jobs(void) {
	pid_t pid;
	int status;	
	int i;

	for (i = 0; i < MAX_JOBS; i++) {
		// Call pid = waitpid() system call properly for each array slot.
		// If this call returns a positive pid, then this means that we just
		// found a terminated background process. Call remove_bg_job()
		// properly to remove this process from the jobs array.
		if (jobs[i].active == 1) {
			pid = waitpid(jobs[i].pid, &status, WNOHANG);
			if (pid > 0) remove_bg_job(pid);
		}
	}	
}

/**
 * @brief Execute all commands in a parsed pipeline.
 *
 * @param[in] pipeline Parsed pipeline.
 *
 * @return 0 on success, -1 on error, -2 if exit was requested.
 */
static int execute_pipeline(const struct pipeline *pipeline)
{
	pid_t *pids;
	int result = 0;
	
	// First, we have to handle the internal commands. To do this, we need to
	// check if our command count is one, and if the name of the command matches
	// an internal command. Remember that the command name is always stored in
	// argv[0].

	if (pipeline != NULL && pipeline->command_count == 1) {
		const struct command *cmd = &pipeline->commands[0];
		const char *cmd_name = cmd->argv[0];

		if (cmd_name != NULL) {
			// Handle exit command
			if (strcmp(cmd_name, "exit") == 0) {
				pipeline_free((struct pipeline *)pipeline);
				exit(EXIT_SUCCESS);
			}

			// Handle cd command
			if (strcmp(cmd_name, "cd") == 0) {
				char *path = cmd->argv[1];
				if (path == NULL) {
					char *home = getenv("HOME");
					if (home == NULL) {
						fprintf(stderr, "cd: HOME env variable not set\n");
						return 0;
					}

					path = home;
				}

				if (chdir(path) < 0) perror("cd");
				return 0;
			}
		}
	}

	// No internal command found. We have to execute a new program for each
	// command in the pipeline.

	// Allocate an array to store the PID of each child process.
    // You may use the array below, if you find it helpful.    
	pids = calloc(pipeline->command_count, sizeof(*pids));
	if (pids == NULL) return -1;

	// Process each command in the pipeline. Remember that the command_count
	// tells us the number of commands in the pipeline.
	// E.g., cmd1 | cmd2 : 2 commands.
	// E.g., cmd1 : 1 command.
	int pipefds[pipeline->command_count > 1 ? 2 * (pipeline->command_count - 1) : 1];
	for (size_t i = 0; i < pipeline->command_count - 1; i++) {
		if (pipe(pipefds + i * 2) < 0) {
			perror("pipe");

			free(pids);
			return -1;
		}
	}

	for (size_t i = 0; i < pipeline->command_count; i++) {
		const struct command *cmd = &pipeline->commands[i];
		const char *cmd_name = cmd->argv[0];
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			free(pids);
			return -1;
		} else if (pid == 0) {
			if (!pipeline->background) {
				struct sigaction dfl;
				memset(&dfl, 0, sizeof(dfl));
				dfl.sa_handler = SIG_DFL;
				sigemptyset(&dfl.sa_mask);
				sigaction(SIGINT, &dfl, NULL);
			}

			if (i > 0) {
				if (dup2(pipefds[(i - 1) * 2], STDIN_FILENO) < 0) {
					perror("dup2 pipe input");
					exit(EXIT_FAILURE);
				}
			}

			if (i < pipeline->command_count - 1) {
				if (dup2(pipefds[i * 2 + 1], STDOUT_FILENO) < 0) {
					perror("minish: dup2 pipe output");
					exit(EXIT_FAILURE);
				}
			}

			for (size_t j = 0; j < 2 * (pipeline->command_count - 1); j++) close(pipefds[j]);

			for (size_t r = 0; r < cmd->redirect_count; r++) {
				const struct redirect *redir = &cmd->redirects[r];
				int fd = -1;

				if (redir->type == REDIRECT_INPUT) {
					fd = open(redir->path, O_RDONLY);
					if (fd < 0) {
						perror("open input");
						exit(EXIT_FAILURE);
					}

					if (dup2(fd, STDIN_FILENO) < 0) {
						perror("dup2 input");
						close(fd);
						exit(EXIT_FAILURE);
					}

					close(fd);
				} else if (redir->type == REDIRECT_OUTPUT) {
					fd = open(redir->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
					if (fd < 0) {
						perror("open output");
						exit(EXIT_FAILURE);
					}

					if (dup2(fd, STDOUT_FILENO) < 0) {
						perror("dup2 output");
						close(fd);
						exit(EXIT_FAILURE);
					}

					close(fd);
				} else if (redir->type == REDIRECT_APPEND) {
					fd = open(redir->path, O_WRONLY | O_CREAT | O_APPEND, 0644);
					if (fd < 0) {
						perror("open append");
						exit(EXIT_FAILURE);
					}

					if (dup2(fd, STDOUT_FILENO) < 0) {
						perror("dup2 append");
						close(fd);
						exit(EXIT_FAILURE);
					}

					close(fd);
				}
			}

			execvp(cmd_name, cmd->argv);

			perror(cmd_name);
			exit(EXIT_FAILURE);
		} else {
			pids[i] = pid;
			if (pipeline->background) add_bg_job(pid);
		}
	}

	for (size_t i = 0; i < 2 * (pipeline->command_count - 1); i++) close(pipefds[i]);

	if(!pipeline->background) for (size_t i = 0; i < pipeline->command_count; i++) waitpid(pids[i], NULL, 0);
        
	free(pids);
	return result;
}
