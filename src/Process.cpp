/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Process.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>


int
RunProgram(const std::vector<BString>& arguments, BString* output)
{
	std::vector<const char*> argv;
	for (const BString& argument : arguments)
		argv.push_back(argument.String());
	argv.push_back(NULL);

	int pipes[2];
	if (pipe(pipes) != 0)
		return -1;
	pid_t child = fork();
	if (child < 0) {
		close(pipes[0]);
		close(pipes[1]);
		return -1;
	}
	if (child == 0) {
		dup2(pipes[1], STDOUT_FILENO);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, STDERR_FILENO);
		close(pipes[0]);
		close(pipes[1]);
		// none of Cinema's other descriptors: a pipe kept open by this
		// program would make another reader wait for its end for ever
		for (int fd = 3, last = getdtablesize(); fd < last; fd++)
			close(fd);
		execvp(argv[0], (char* const*)argv.data());
		_exit(127);
	}

	close(pipes[1]);
	char buffer[4096];
	ssize_t bytes;
	while ((bytes = read(pipes[0], buffer, sizeof(buffer))) > 0) {
		if (output != NULL)
			output->Append(buffer, bytes);
	}
	close(pipes[0]);

	int status;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}
