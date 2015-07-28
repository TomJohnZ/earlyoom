/* Check available memory and swap in a loop and start killing
 * processes if they get too low */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <regex.h>

#include "meminfo.h"
#include "kill.h"

int enable_debug = 0;

// Any process whose cmdline matches this regexp will be LESS likely to be killed.
char *excluded_cmdlines_pattern = "(^|/)(((init|X|sshd|firefox)( .*|$))|chrome|chromium-browser)$";
// (I want to match all init, sshd and firefox processes, but only the INITIAL chrome process.  Chrome tab processes and extension processes will not be protected.)

// Any process whose cmdline matches this regexp will be MORE likely to be killed.
char *preferred_cmdlines_pattern = "(^|/)(chrome|chromium-browser).*--type=renderer";
// (I want to kill Chrome tabs more often than Chrome extensions.)

regex_t excluded_cmdlines_regexp;
regex_t preferred_cmdlines_regexp;

int main(int argc, char *argv[])
{
	int kernel_oom_killer = 0;
	unsigned long oom_cnt = 0;
	/* If the available memory goes below this percentage, we start killing
	 * processes. 10 is a good start. */
	int mem_min_percent = 10, swap_min_percent = 10;
	long mem_min, swap_min; /* Same thing in kiB */
	int ignore_oom_score_adj = 0;

	if (regcomp(&excluded_cmdlines_regexp, excluded_cmdlines_pattern, REG_EXTENDED|REG_NOSUB) != 0)
	{
		fprintf(stderr, "Could not compile regexp: %s\n", excluded_cmdlines_pattern);
		exit(6);
	}

	if (regcomp(&preferred_cmdlines_regexp, preferred_cmdlines_pattern, REG_EXTENDED|REG_NOSUB) != 0)
	{
		fprintf(stderr, "Could not compile regexp: %s\n", excluded_cmdlines_pattern);
		exit(6);
	}

	/* request line buffering for stdout - otherwise the output
	 * may lag behind stderr */
	setlinebuf(stdout);

	fprintf(stderr, "earlyoom %s\n", GITVERSION);

	if(chdir("/proc")!=0)
	{
		perror("Could not cd to /proc");
		exit(4);
	}

	DIR *procdir = opendir(".");
	if(procdir==NULL)
	{
		perror("Could not open /proc");
		exit(5);
	}

	int c;
	while((c = getopt (argc, argv, "m:s:kidh")) != -1)
	{
		switch(c)
		{
			case 'm':
				mem_min_percent = strtol(optarg, NULL, 10);
				if(mem_min_percent <= 0) {
					fprintf(stderr, "-m: Invalid percentage\n");
					exit(15);
				}
				break;
			case 's':
				swap_min_percent = strtol(optarg, NULL, 10);
				if(swap_min_percent <= 0 || swap_min_percent > 100) {
					fprintf(stderr, "-s: Invalid percentage\n");
					exit(16);
				}
				break;
			case 'k':
				kernel_oom_killer = 1;
				fprintf(stderr, "Using kernel oom killer\n");
				break;
			case 'i':
				ignore_oom_score_adj = 1;
				break;
			case 'd':
				enable_debug = 1;
				break;
			case 'h':
				fprintf(stderr,
					"Usage: earlyoom [-m PERCENT] [-s PERCENT] [-k|-i] [-h]\n"
					"-m ... set available memory minimum to PERCENT of total (default 10 %%)\n"
					"-s ... set free swap minimum to PERCENT of total (default 10 %%)\n"
					"-k ... use kernel oom killer instead of own user-space implementation\n"
					"-i ... user-space oom killer should ignore positive oom_score_adj values\n"
					"-d ... enable debugging messages\n"
					"-h ... this help text\n");
				exit(1);
			case '?':
				exit(13);
		}
	}

	if(kernel_oom_killer && ignore_oom_score_adj) {
		fprintf(stderr, "Kernel oom killer does not support -i\n");
		exit(2);
	}

	struct meminfo m = parse_meminfo();
	mem_min = m.MemTotal * mem_min_percent / 100;
	swap_min = m.SwapTotal * swap_min_percent / 100;

	fprintf(stderr, "mem total: %lu MiB, min: %lu MiB (%d %%)\n",
		m.MemTotal / 1024, mem_min / 1024, mem_min_percent);
	fprintf(stderr, "swap total: %lu MiB, min: %lu MiB (%d %%)\n",
		m.SwapTotal / 1024, swap_min / 1024, swap_min_percent);

	/* Dry-run oom kill to make sure stack grows to maximum size before
	 * calling mlockall()
	 */
	handle_oom(procdir, 0, kernel_oom_killer, ignore_oom_score_adj);

	if(mlockall(MCL_FUTURE)!=0)
	{
		perror("Could not lock memory");
		exit(10);
	}

	c = 1; // Start at 1 so we do not print another status line immediately
	while(1)
	{
		m = parse_meminfo();

		if(c % 10 == 0)
		{
			printf("mem avail: %5lu MiB, swap free: %5lu MiB\n",
				m.MemAvailable / 1024, m.SwapFree / 1024);
			c=0;
		}
		c++;

		if(m.MemAvailable <= mem_min && m.SwapFree <= swap_min)
		{
			fprintf(stderr, "Out of memory! avail: %lu MiB < min: %lu MiB\n",
				m.MemAvailable / 1024, mem_min / 1024);
			handle_oom(procdir, 9, kernel_oom_killer, ignore_oom_score_adj);
			oom_cnt++;
		}
		
		usleep(100000); // 100ms
	}
	
	return 0;
}
