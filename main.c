/*
	C port of the fancontrol shell script implementing
	a temperature dependent fan speed control

	Copyright 2017 <sfan5@live.de>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
	MA 02110-1301 USA.
*/

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>


#define PIDFILE "/var/run/fancontrol.pid"
#define CONFIGFILE "/etc/fancontrol"
#define MAXMAPPINGS 32
#define SECS_TO_US (1000 * 1000)

#define startswith(s1, s2) ( strncmp(s1, s2, strlen(s2)) == 0 )
#define my_strdup(s) ({ char *r=strdup(s);if(!r)exit(2);r; })
#define my_strndup(s,n) ({ char *r=strndup(s,n);if(!r)exit(2);r; })
#define atoi_and_free(s) ({ int n=s?atoi(s):0;free(s);n; })
#define copy_from_config(line, prefix, where) do { \
	if(startswith(line, prefix)) \
		where = my_strdup(&line[strlen(prefix)]); \
	} while(0)


static struct {
	int interval;
	char *devpath, *devname;
	struct {
		char *pwm;
		char *temp;
		char *fan;
		int mintemp, maxtemp;
		int minstart, minstop;
		int minpwm, maxpwm;
	} afc[MAXMAPPINGS];
} config;


static char *get_matching_part(const char *full, char *key)
{
	if(!full)
		return NULL;
	const char *ptr = full;
	char search[64];
	snprintf(search, sizeof(search), "%s=", key);
	while(*ptr) {
		if(startswith(ptr, search)) {
			const char *vptr = &ptr[strlen(search)], *tmp = vptr;
			int n = 0;
			while(*tmp && *(tmp++) != ' ')
				n++;
			return my_strndup(vptr, n);
		}
		while(*ptr && *ptr != ' ')
			ptr++;
		if(*ptr)
			ptr++;
	}
	return NULL;
}

static int check_i2c_regex(const char *s)
{
	// TODO: consider checking properly using '^[1-9]*[0-9]-[0-9abcdef]{4}'
	// this checks '^[0-9]+-' to detect i2c device names
	const char *ptr = s;
	while(*ptr >= '0' && *ptr <= '9')
		ptr++;
	return ptr > s && *ptr == '-';
}

static int wfile(const char *path, int value)
{
	int fd = open(path, O_WRONLY | O_CREAT, 0644);
	if(fd == -1)
		return -1;
	char tmp[12];
	snprintf(tmp, sizeof(tmp), "%d", value);
	write(fd, tmp, strlen(tmp));
	close(fd);
	return 0;
}

static int rfile(const char *path)
{
	int fd = open(path, O_RDONLY);
	if(fd == -1)
		return -1;
	char tmp[12] = {0}, *p;
	read(fd, tmp, sizeof(tmp));
	close(fd);
	int ret = strtol(tmp, &p, 10);
	return (tmp == p) ? -1 : ret;
}

void restorefans(int status);
static void sighandler(int signal)
{
	restorefans( (signal == SIGQUIT || signal == SIGTERM) ? 0 : 1 );
}

static const char *my_getcwd(void)
{
	static char buf[512];
	getcwd(buf, sizeof(buf));
	return buf;
}

/****/

int LoadConfig(const char *path)
{
	config.interval = -1;
	config.devpath = config.devname = NULL;
	for(int i = 0; i < MAXMAPPINGS; i++) {
		config.afc[i].pwm = NULL;
		// default values for optional settings:
		config.afc[i].fan = NULL;
		config.afc[i].minpwm = 0;
		config.afc[i].maxpwm = 255;
	}

	char *fctemps, *mintemp, *maxtemp, *minstart, *minstop, *fcfans, *minpwm, *maxpwm;
	fctemps = mintemp = maxtemp = minstart = minstop = fcfans = minpwm = maxpwm = NULL;

	FILE *f;
	printf("Loading configuration from %s...\n", path);
	f = fopen(path, "r");
	if(!f) {
		fprintf(stderr, "Error: Can't read configuration file: %s\n", strerror(errno));
		return -1;
	}
	while(1) {
		char line[512];
		if(!fgets(line, sizeof(line), f))
			break;
		if(*line == '#')
			continue;
		if(line[strlen(line) - 1] == '\n')
			line[strlen(line) - 1] = '\0';

		if(startswith(line, "INTERVAL="))
			config.interval = atoi(&line[strlen("INTERVAL=")]);
		copy_from_config(line, "DEVPATH=", config.devpath);
		copy_from_config(line, "DEVNAME=", config.devname);
		copy_from_config(line, "FCTEMPS=", fctemps);
		copy_from_config(line, "MINTEMP=", mintemp);
		copy_from_config(line, "MAXTEMP=", maxtemp);
		copy_from_config(line, "MINSTART=", minstart);
		copy_from_config(line, "MINSTOP=", minstop);
		// optional settings:
		copy_from_config(line, "FCFANS=", fcfans);
		copy_from_config(line, "MINPWM=", minpwm);
		copy_from_config(line, "MAXPWM=", maxpwm);
	}
	fclose(f);

	if(config.interval <= 0 || !fctemps || !mintemp || !maxtemp || !minstart || !minstop) {
		fprintf(stderr, "Some mandatory settings missing, please check your config file!\n");
		return -1;
	}
	printf("\nCommon settings:\n");
	printf("  INTERVAL=%d\n", config.interval);

	char *ptr = strtok(fctemps, " ");
	int fcvcount = 0;
	do {
		if(!strchr(ptr, '=')) {
			fprintf(stderr, "Config error: FCTEMPS value is improperly formatted\n");
			return -1;
		}

		char *key = my_strndup(ptr, strchr(ptr, '=') - ptr);
		config.afc[fcvcount].pwm = key;
		config.afc[fcvcount].temp = my_strdup(strchr(ptr, '=') + 1);
		if(fcfans) {
			config.afc[fcvcount].fan = get_matching_part(fcfans, key);
			if(config.afc[fcvcount].fan && strchr(config.afc[fcvcount].fan, '+')) {
				// TODO
				fprintf(stderr, "Config error: Multiple fans per input currently unsupported!\n");
				return -1;
			}
		}
		config.afc[fcvcount].mintemp = atoi_and_free(get_matching_part(mintemp, key));
		config.afc[fcvcount].maxtemp = atoi_and_free(get_matching_part(maxtemp, key));
		config.afc[fcvcount].minstart = atoi_and_free(get_matching_part(minstart, key));
		config.afc[fcvcount].minstop = atoi_and_free(get_matching_part(minstop, key));
		char *tmp;
		if(( tmp = get_matching_part(minpwm, key) ))
			config.afc[fcvcount].minpwm = atoi_and_free(tmp);
		if(( tmp = get_matching_part(maxpwm, key) ))
			config.afc[fcvcount].maxpwm = atoi_and_free(tmp);

		// verify the validity of the settings
		if(config.afc[fcvcount].mintemp >= config.afc[fcvcount].maxtemp) {
			fprintf(stderr, "Config error (%s): MINTEMP must be less than MAXTEMP\n", key);
			return -1;
		}
		if(config.afc[fcvcount].maxpwm > 255) {
			fprintf(stderr, "Config error (%s): MAXPWM must be at most 255\n", key);
			return -1;
		}
		if(config.afc[fcvcount].minstop >= config.afc[fcvcount].maxpwm) {
			fprintf(stderr, "Config error (%s): MINSTOP must be less than MAXPWM\n", key);
			return -1;
		}
		if(config.afc[fcvcount].minstop < config.afc[fcvcount].minpwm) {
			fprintf(stderr, "Config error (%s): MINSTOP must be greater than or equal to MINPWM\n", key);
			return -1;
		}
		if(config.afc[fcvcount].minpwm < 0) {
			fprintf(stderr, "Config error (%s): MINPWM must be at least 0\n", key);
			return -1;
		}

		printf("Settings for %s:\n", key);
		printf("  Depends on %s\n", config.afc[fcvcount].temp);
		printf("  Controls %s\n", config.afc[fcvcount].fan);
		printf("  MINTEMP=%d\n", config.afc[fcvcount].mintemp);
		printf("  MAXTEMP=%d\n", config.afc[fcvcount].maxtemp);
		printf("  MINSTART=%d\n", config.afc[fcvcount].minstart);
		printf("  MINSTOP=%d\n", config.afc[fcvcount].minstop);
		printf("  MINPWM=%d\n", config.afc[fcvcount].minpwm);
		printf("  MAXPWM=%d\n", config.afc[fcvcount].maxpwm);

		fcvcount++;
	} while(( ptr = strtok(NULL, " ") ));

	free(fctemps); free(mintemp); free(maxtemp);
	free(minstart); free(minstop); free(fcfans); free(minpwm); free(maxpwm);
	printf("\n");
	return 0;
}

void FixupDeviceFiles(const char *device)
{
	char search[64];
	snprintf(search, sizeof(search), "%s/device", device);
	for(int i = 0; i < MAXMAPPINGS; i++) {
		if(!config.afc[i].pwm)
			continue;

		// go through pwm output & temp input & fan input
		// and replace <device>/device with <device>
		if(strstr(config.afc[i].pwm, search)) {
			char *tmp, *tmp2;
			tmp = strstr(config.afc[i].pwm, search) + strlen(device);
			tmp2 = tmp + strlen("/device");
			printf("Adjusting %s -> ", config.afc[i].pwm);
			memmove(tmp, tmp2, strlen(tmp2) + 1);
			printf("%s\n", config.afc[i].pwm);
		}
		if(strstr(config.afc[i].temp, search)) {
			char *tmp, *tmp2;
			tmp = strstr(config.afc[i].temp, search) + strlen(device);
			tmp2 = tmp + strlen("/device");
			printf("Adjusting %s -> ", config.afc[i].temp);
			memmove(tmp, tmp2, strlen(tmp2) + 1);
			printf("%s\n", config.afc[i].temp);
		}
		if(strstr(config.afc[i].fan, search)) {
			char *tmp, *tmp2;
			tmp = strstr(config.afc[i].fan, search) + strlen(device);
			tmp2 = tmp + strlen("/device");
			printf("Adjusting %s -> ", config.afc[i].fan);
			memmove(tmp, tmp2, strlen(tmp2) + 1);
			printf("%s\n", config.afc[i].fan);
		}
	}
}

// Some drivers moved their attributes from hard device to class device
void FixupFiles(void)
{
	char *ptr = strtok(config.devpath, " ");
	do {
		char *save = strchr(ptr, '='), tmp[64];
		*save = '\0';

		snprintf(tmp, sizeof(tmp), "%s/name", ptr);
		if(access(tmp, F_OK) == 0)
			FixupDeviceFiles(ptr);

		*save = '=';
	} while(( ptr = strtok(NULL, " ") ));
}

// Check that all referenced sysfs files exist
int CheckFiles(void)
{
	int outdated = 0;

	for(int i = 0; i < MAXMAPPINGS; i++) {
		if(!config.afc[i].pwm)
			continue;

		// go through pwm output & temp input & fan input
		if(access(config.afc[i].pwm, W_OK) == -1) {
			fprintf(stderr, "Error: File %s doesn't exist or isn't writable\n", config.afc[i].pwm);
			outdated = 1;
		}
		if(access(config.afc[i].temp, R_OK) == -1) {
			fprintf(stderr, "Error: File %s doesn't exist\n", config.afc[i].temp);
			outdated = 1;
		}
		if(access(config.afc[i].fan, R_OK) == -1) {
			fprintf(stderr, "Error: File %s doesn't exist\n", config.afc[i].fan);
			outdated = 1;
		}
	}

	if(outdated) {
		fprintf(stderr, "\n"
			"At least one referenced file is missing. Either some required kernel\n"
			"modules haven't been loaded, or your configuration file is outdated.\n"
			"In the latter case, you should run pwmconfig again.\n"
		);
	}
	return outdated ? -1 : 0;
}

/****/

int pwmdisable(const char *name /*pwm file name*/)
{
	char enable[64];
	snprintf(enable, sizeof(enable), "%s_enable", name);

	// No enable file? Just set to max
	if(access(enable, F_OK) == -1) {
		wfile(name, 255);
		return 0;
	}

	// Try pwmN_enable=0
	wfile(enable, 0);
	if(rfile(enable) == 0)
		return 0; // Success

	// It didn't work, try pwmN_enable=1 pwmN=255
	wfile(enable, 1);
	wfile(name, 255);
	int read = rfile(enable);
	if(read == 1 && rfile(name) >= 190)
		return 0; // Success

	fprintf(stderr, "%s stuck to %d\n", enable, read);
	return -1;
}

int pwmenable(const char *name /*pwm file name*/)
{
	char enable[64];
	snprintf(enable, sizeof(enable), "%s_enable", name);

	if(access(enable, F_OK) == 0) {
		if(wfile(enable, 1) == -1)
			return -1;
	}
	wfile(name, 255);
	return 0;
}

void restorefans(int status)
{
	printf("Aborting, restoring fans...\n");
	for(int i = 0; i < MAXMAPPINGS; i++) {
		if(!config.afc[i].pwm)
			continue;
		pwmdisable(config.afc[i].pwm);
	}
	printf("Verify fans have returned to full speed\n");
	unlink(PIDFILE);
	exit(status);
}

// main function
void UpdateFanSpeeds(void)
{
	// go through all pwm outputs
	for(int i = 0; i < MAXMAPPINGS; i++) {
		if(!config.afc[i].pwm)
			continue;
		// hopefully shorter vars will improve readability:
		const char *pwmo, *tsens, *fan;
		int mint, maxt, minsa, minso, minpwm, maxpwm;
		pwmo = config.afc[i].pwm;
		tsens = config.afc[i].temp;
		fan = config.afc[i].fan;
		mint = config.afc[i].mintemp * 1000;
		maxt = config.afc[i].maxtemp * 1000;
		minsa = config.afc[i].minstart;
		minso = config.afc[i].minstop;
		minpwm = config.afc[i].minpwm;
		maxpwm = config.afc[i].maxpwm;

		int tval = rfile(tsens);
		if(tval == -1) {
			fprintf(stderr, "Error reading temperature from %s/%s\n", my_getcwd(), tsens);
			restorefans(1);
		}

		int pwmpval = rfile(pwmo);
		if(pwmpval == -1) {
			fprintf(stderr, "Error PWM value from %s/%s\n", my_getcwd(), pwmo);
			restorefans(1);
		}

		// If fanspeed-sensor output shall be used, do it
		int min_fanval;
		if(fan) {
			min_fanval = rfile(fan);
			if(min_fanval == -1) {
				fprintf(stderr, "Error reading Fan value from %s/%s\n", my_getcwd(), fan);
				restorefans(1);
			}
		} else {
			min_fanval = 1; // set it to a non zero value, so the rest of the script still works
		}

		int pwmval;
		if(tval <= mint)
			pwmval = minpwm; // below min temp, use defined min pwm
		else if(tval >= maxt)
			pwmval = maxpwm; // over max temp, use defined max pwm
		else {
			// calculate the new value from temperature and settings
			pwmval = (tval - mint) * (maxpwm - minso) / (maxt - mint) + minso;
			if(pwmpval == 0 || min_fanval == 0) {
				// if fan was stopped start it using a safe value
				wfile(pwmo, minsa);
				usleep(1 * SECS_TO_US);
			}
		}
		// write new value to pwm output
		if(wfile(pwmo, pwmval) == -1) {
			fprintf(stderr, "Error writing PWM value to %s/%s\n", my_getcwd(), pwmo);
			restorefans(1);
		}
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 256);

	if(argc > 1 && access(argv[1], F_OK) == 0) {
		if(LoadConfig(argv[1]) == -1)
			return 1;
	} else {
		if(LoadConfig(CONFIGFILE) == -1)
			return 1;
	}

	// Detect path to sensors
	const char *dir = NULL;
	if(*config.afc[0].pwm == '/')
		dir = "/";
	else if(startswith(config.afc[0].pwm, "hwmon"))
		dir = "/sys/class/hwmon";
	else if(check_i2c_regex(config.afc[0].pwm))
		dir = "/sys/bus/i2c/devices";
	if(!dir) {
		fprintf(stderr, "Invalid path to sensors\n");
		return 1;
	}

	struct stat s;
	if(stat(dir, &s) == -1 || !S_ISDIR(s.st_mode)) {
		fprintf(stderr, "No sensors found! (did you load the necessary modules?)\n");
		return 1;
	}
	chdir(dir);

	if(!strcmp(dir, "/sys/class/hwmon"))
		FixupFiles();
	if(CheckFiles() == -1)
		return 1;
	free(config.devpath); free(config.devname);

	if(access(PIDFILE, F_OK) == 0) {
		fprintf(stderr, "File %s exists, is fancontrol already running?\n", PIDFILE);
		return 1;
	}
	wfile(PIDFILE, getpid());

	signal(SIGQUIT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGHUP, sighandler);
	signal(SIGINT, sighandler);

	printf("Enabling PWM on fans...\n");
	for(int i = 0; i < MAXMAPPINGS; i++) {
		if(!config.afc[i].pwm)
			continue;
		int r = pwmenable(config.afc[i].pwm);
		if(r == -1) {
			fprintf(stderr, "Error enabling PWM on %s/%s", my_getcwd(), config.afc[i].pwm);
			restorefans(1);
		}
	}

	printf("Starting automatic fan control...\n");

	while(1) {
		UpdateFanSpeeds();
		usleep(config.interval * SECS_TO_US);
	}
}
