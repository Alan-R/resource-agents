/******************************************************************************
*******************************************************************************
**
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "log.h"
#include "debug.h"
#include "cnx_mgr.h"
#include "cluster_mgr.h"
#include "globals.h"

#include "copyright.cf"

int parent_exit_code=-1;

static unsigned int flags=0;
#define FLAG_NODAEMON	1
#define FLAG_VERBOSE	2

static char *parse_cli_args(int argc, char *argv[]);
static void daemonize(void);
static void print_start_msg(char *msg);
static int join_group(int sfd, int loopback, int port);

int main(int argc, char *argv[]){
  int i,error=0;
  int trueint = 1;
  int sfds[2] = {-1,-1}, afd;
  struct sockaddr_storage addr;
  struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
  struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
  int addr_size=0;
  fd_set rset, tmp_set;
  char *msg;

  msg = parse_cli_args(argc, argv);

  daemonize();

  print_start_msg(msg);

  if(msg){
    free(msg);
  }

  if(start_cluster_monitor_thread()){
    log_err("Unable to create thread.\n");
    exit(EXIT_FAILURE);
  }

  /** Setup the socket to communicate with the CCS library **/
  if(IPv6 && (sfds[0] = socket(PF_INET6, SOCK_STREAM, 0)) < 0){
    if(IPv6 == -1){
      log_dbg("Unable to create IPv6 socket:: %s\n", strerror(errno));
      IPv6=0;
    } else {
      log_sys_err("Unable to create IPv6 socket");
      exit(EXIT_FAILURE);
    }
  } else {
    /* IPv6 is no longer optional for ccsd
    IPv6 = (IPv6)? 1: 0;
    */
  }

  log_dbg("Using %s\n", IPv6?"IPv6":"IPv4");

  if(!IPv6 && (sfds[0] = socket(PF_INET, SOCK_STREAM, 0)) < 0){
    log_sys_err("Unable to create IPv4 socket");
    exit(EXIT_FAILURE);
  }

  if(setsockopt(sfds[0], SOL_SOCKET, SO_REUSEADDR, &trueint, sizeof(int))){
    log_sys_err("Unable to set socket option");
    exit(EXIT_FAILURE);
  }

  if(IPv6){
    addr_size = sizeof(struct sockaddr_in6);
    addr6->sin6_family = AF_INET6;
    addr6->sin6_addr = in6addr_loopback;
    addr6->sin6_port = htons(frontend_port);
  } else {
    addr_size = sizeof(struct sockaddr_in);
    addr4->sin_family = AF_INET;
    /*    addr4->sin_addr.s_addr = INADDR_LOOPBACK; */
    inet_aton("127.0.0.1", (struct in_addr *)&(addr4->sin_addr.s_addr));
    addr4->sin_port = htons(frontend_port);
  }
 
  if(bind(sfds[0], (struct sockaddr *)&addr, addr_size) < 0){
    log_sys_err("Unable to bind socket");
    close(sfds[0]);
    exit(EXIT_FAILURE);
  }
 
  listen(sfds[0], 5);


  /** Setup the socket to communicate with the CCS library **/
  sfds[1] = socket((IPv6)? PF_INET6: PF_INET, SOCK_DGRAM, 0);
  if(sfds[1] < 0){
    log_sys_err("Socket creation failed");
    exit(EXIT_FAILURE);
  } else {
    int trueint = 1;
    if(setsockopt(sfds[1], SOL_SOCKET, SO_REUSEADDR, &trueint, sizeof(int))){
      log_sys_err("Unable to set socket option");
      exit(EXIT_FAILURE);
    }  
  }

  if(IPv6){
    addr6->sin6_family = AF_INET6;
    addr6->sin6_addr = in6addr_any;
    addr6->sin6_port = htons(backend_port);
  } else {
    addr4->sin_family = AF_INET;
    addr4->sin_addr.s_addr = INADDR_ANY;
    addr4->sin_port = htons(backend_port);
  }
 
  if(bind(sfds[1], (struct sockaddr *)&addr, addr_size) < 0){
    log_sys_err("Unable to bind socket");
    close(sfds[1]);
    return -errno;
  }

  if(IPv6 || multicast_address){
    if(join_group(sfds[1], 1, backend_port)){
      log_err("Unable to join multicast group.\n");
      exit(EXIT_FAILURE);
    }
  }
 
  listen(sfds[1], 5);

  FD_ZERO(&rset);
  FD_SET(sfds[0], &rset);
  FD_SET(sfds[1], &rset);

  while(1){
    int len = addr_size;
    
    tmp_set = rset;

    if((select(FD_SETSIZE, &tmp_set, NULL,NULL,NULL) < 0)){
      log_sys_err("Select failed");
      continue;
    }
    
    for(i=0; i<2; i++){
      if(!FD_ISSET(sfds[i], &tmp_set)){
	continue;
      }
      if(i == 0){
	uint16_t port;
	log_dbg("NORMAL CCS REQUEST.\n");
	afd = accept(sfds[i], (struct sockaddr *)&addr, &len);
	if(afd < 0){
	  log_sys_err("Unable to accept connection");
	  continue;
	}

	port = (IPv6) ? addr6->sin6_port : addr4->sin_port;

	log_dbg("Connection requested from port %u.\n", ntohs(port));

	if(ntohs(port) > 1024){
	  log_err("Refusing connection from port > 1024:  port = %d", ntohs(port));
	  close(afd);
	  continue;
	}
	if((error = process_request(afd))){
	  log_err("Error while processing request: %s\n", strerror(-error));
	}
	close(afd);
      } else {
	log_dbg("BROADCAST REQUEST.\n");
	if((error = process_broadcast(sfds[i]))){
	  log_err("Error while processing broadcast: %s\n", strerror(-error));
	}
      }
    }
  }
  exit(EXIT_SUCCESS);
}


/**
 * print_usage - print usage information
 * @stream: open file stream to print to
 *
 */
static void print_usage(FILE *stream){
  ENTER("print_usage");
  fprintf(stream,
	  "Usage:\n"
	  "\n"
	  "ccsd [Options]\n"
	  "\n"
	  "Options:\n"
	  " -4	          Use IPv4 only.\n"
	  " -6	          Use IPv6 only.\n"
	  " -h            Help.\n"
	  " -m <addr>     Specify multicast address (\"default\" ok).\n"
	  " -n            No Daemon.  Run in the foreground.\n"
	  /*	  " -p <file>     Specify the location of the pid file.\n"*/
	  " -t <ttl>      Multicast threshold (aka Time to Live) value.\n"
	  " -P [bcf]:#    Specify various port numbers.\n"
	  " -V            Print version information.\n"
	  " -v            Verbose.\n"
	  );
  EXIT("print_usage");
}


static int is_multicast_addr(char *addr_string){
  int rtn = 0;
  struct sockaddr_storage addr;
  struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
  struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;

  ENTER("is_multicast_addr");

  if(inet_pton(AF_INET6, addr_string, &(addr6->sin6_addr)) > 0){
    if(IN6_IS_ADDR_MULTICAST(&addr6->sin6_addr)){
      rtn = AF_INET6;
    }
  } else if(inet_pton(AF_INET, addr_string, &(addr4->sin_addr)) > 0){
    if(IN_MULTICAST(ntohl(addr4->sin_addr.s_addr))){
      rtn = AF_INET;
    }
  }
  EXIT("is_multicast_addr");
  return rtn;
}


/**
 * parse_cli_args
 * @argc:
 * @argv:
 *
 * This function parses the command line arguments and sets the
 * appropriate flags in the global 'flags' variable.  Additionally,
 * it sets the global 'config_file_location'.  This function
 * will either succeed or cause the program to exit.
 *
 * Returns: string (or NULL) describing changes, exit(EXIT_FAILURE) on error
 */
static char *parse_cli_args(int argc, char *argv[]){
  int c, error=0;
  int buff_size=512;
  char buff[buff_size];
  int buff_index=0;

  ENTER("parse_cli_args");

  config_file_location = strdup(DEFAULT_CONFIG_LOCATION);
  lockfile_location = strdup(DEFAULT_CCSD_LOCKFILE);

  if(!config_file_location || !lockfile_location){
    fprintf(stderr, "Insufficient memory.\n");
    error = -ENOMEM;
    goto fail;
  }

  memset(buff, 0, buff_size);

  while((c = getopt(argc, argv, "46cdf:hlm:nP:t:sVv")) != -1){
    switch(c){
    case '4':
      if(IPv6 == 1){
	fprintf(stderr, "Setting protocol to IPv4 conflicts with multicast address.\n");
	error = -EINVAL;
	goto fail;
      }
      IPv6=0;
      buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			     "  IP Protocol:: IPv4 only\n");
      break;
    case '6':
      if(IPv6 == 0){
	fprintf(stderr, "Setting protocol to IPv6 conflicts with multicast address.\n");
	error = -EINVAL;
	goto fail;
      }
      IPv6=1;
      buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			     "  IP Protocol:: IPv6 only\n");
      break;
    case 'c':
      fprintf(stderr, "The '-c' option is depricated.\n"
	      "Try '-h' for help.\n");
      error = -EINVAL;
      goto fail;
    case 'd':  /* might be usable for upgrade */
      fprintf(stderr, "The '-d' option is depricated.\n"
	      "Try '-h' for help.\n");
      error = -EINVAL;
      goto fail;
    case 'f':  /* might be usable for upgrade */
      free(config_file_location);
      config_file_location = optarg;
      buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			     "  Config file location:: %s\n", optarg);
      break;
    case 'h':
      print_usage(stdout);
      exit(EXIT_SUCCESS);
    case 'l':
      fprintf(stderr, "The '-l' option is depricated.\n"
	      "Try '-h' for help.\n");
      error = -EINVAL;
      goto fail;
    case 'm':
      if(strcmp("default", optarg)){
	int type = is_multicast_addr(optarg);
	if((IPv6 == 1) && (type != AF_INET6)){
	  fprintf(stderr, "%s is not a valid IPv6 multicast address.\n", optarg);
	  error = -EINVAL;
	  goto fail;
	} else if((IPv6 == 0) && (type != AF_INET)){
	  fprintf(stderr, "%s is not a valid IPv4 multicast address.\n", optarg);
	  error = -EINVAL;
	  goto fail;
	} else if(type == 0){
	  fprintf(stderr, "%s is not a valid multicast address.\n", optarg);
	  error = -EINVAL;
	  goto fail;
	} else {
	  IPv6 = (type == AF_INET6)? 1: 0;
	  buff_index += snprintf(buff+buff_index, buff_size-buff_index,
				 "  IP Protocol:: %s only*\n",
				 (IPv6)? "IPv6" : "IPv4");
	}
      }
      multicast_address = optarg;
      buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			     "  Multicast (%s):: SET\n", optarg);
      break;
    case 'n':
      flags |= FLAG_NODAEMON;
      buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			     "  No Daemon:: SET\n");
      break;
    case 'p':
      free(lockfile_location);
      lockfile_location = optarg;
      buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			     "  Lock file location:: %s\n", optarg);
    case 'P':
      if(optarg[1] != ':'){
	fprintf(stderr, "Bad argument to '-P' option.\n"
		"Try '-h' for help.\n");
	error = -EINVAL;
	goto fail;
      }
      switch(optarg[0]){
      case 'b': /* backend port number */
	backend_port = atoi(optarg+2);
	buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			       "  Backend Port:: %d\n", backend_port);
	break;
      case 'c': /* cluster base port number */
	cluster_base_port = atoi(optarg+2);
	buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			       "  Cluster base port:: %d\n", cluster_base_port);
	break;
      case 'f': /* frontend port number */
	frontend_port = atoi(optarg+2);
	buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			       "  Frontend Port:: %d\n", frontend_port);
	break;
      default:
	fprintf(stderr, "Bad argument to '-P' option.\n"
		"Try '-h' for help.\n");
	error = -EINVAL;
	goto fail;
      }
      break;
    case 's':
      fprintf(stderr, "The '-s' option is depricated.\n"
	      "Try '-h' for help.\n");
      error = -EINVAL;
      goto fail;
    case 't':
      ttl = atoi(optarg);
      break;
    case 'V':
      printf("%s %s (built %s %s)\n", argv[0], CCS_RELEASE_NAME, __DATE__, __TIME__);
      printf("%s\n", REDHAT_COPYRIGHT);
      exit(EXIT_SUCCESS);
    case 'v':
      flags |= FLAG_VERBOSE;
      buff_index += snprintf(buff+buff_index, buff_size-buff_index,
			     "  Verbose Flag:: SET\n");
      break;
    default:
      print_usage(stderr);
      error = -EINVAL;
      goto fail;
    }
  }

 fail:
  EXIT("parse_cli_args");

  if(error){
    exit(EXIT_FAILURE);
  }
  if(strlen(buff)){
    return(strdup(buff));
  } else {
    return NULL;
  }
}


/**
 * create_lockfile - create and lock a lock file
 * @lockfile: location of lock file
 *
 * Returns: 0 on success, -1 otherwise
 */
static int create_lockfile(char *lockfile){
  int fd, error=0;
  struct stat stat_buf;
  struct flock lock;
  char buffer[50];

  ENTER("create_lockfile");
   
  if(!strncmp(lockfile, "/var/run/sistina/", 17)){
    if(stat("/var/run/sistina", &stat_buf)){
      if(mkdir("/var/run/sistina", S_IRWXU)){
        log_sys_err("Cannot create lockfile directory");
        error = -errno;
	goto fail;
      }
    } else if(!S_ISDIR(stat_buf.st_mode)){
      log_err("/var/run/sistina is not a directory.\n"
              "Cannot create lockfile.\n");
      error = -ENOTDIR;
      goto fail;
    }
  }
 
  if((fd = open(lockfile, O_CREAT | O_WRONLY,
                (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH))) < 0){
    log_sys_err("Cannot create lockfile");
    error = -errno;
    goto fail;
  }
 
  lock.l_type = F_WRLCK;
  lock.l_start = 0;
  lock.l_whence = SEEK_SET;
  lock.l_len = 0;
 
  if (fcntl(fd, F_SETLK, &lock) < 0) {
    close(fd);
    log_err("The ccsd process is already running.\n");
    error = -errno;
    goto fail;
  }
 
  if (ftruncate(fd, 0) < 0) {
    close(fd);
    error = -errno;
    goto fail;
  }
 
  sprintf(buffer, "%d\n", getpid());
 
  if(write(fd, buffer, strlen(buffer)) < strlen(buffer)){
    close(fd);
    unlink(lockfile);
    error = -errno;
    goto fail;
  }

 fail: 
  EXIT("create_lockfile");
  
  /* leave fd open - rely on exit to close it */
  if(error){
    return error;
  } else {
    return 0;
  }
}


/**
 * parent_exit_handler: exit the parent
 * @sig: the signal
 *
 * This function allows us to send the parent process any
 * failure notices that might occur in the child when daemonizing.
 * This way, the user can be notified by the proper process.
 */
static void parent_exit_handler(int sig){
  ENTER("parent_exit_handler");

  switch(sig){
  case SIGUSR1:
    fprintf(stderr, "Failed to create lock file.\n");
    parent_exit_code = EXIT_FAILURE;
    break;
  default:
    parent_exit_code = EXIT_SUCCESS;
  }

  EXIT("parent_exit_handler");
}


/**
 * sig_handler
 * @sig
 *
 * This handles signals which the daemon might receive.
 */
static void sig_handler(int sig){
  int err;

  ENTER("sig_handler");

  switch(sig){
  case SIGINT:
    log_msg("Stopping ccsd, SIGINT received.\n");
    err = EXIT_SUCCESS;
    break;
  case SIGQUIT:
    log_msg("Stopping ccsd, SIGQUIT received.\n");
    err = EXIT_SUCCESS;
    break;
  case SIGTERM:
    log_msg("Stopping ccsd, SIGTERM received.\n");
    err = EXIT_SUCCESS;
    break;
  case SIGSEGV:
    log_err("Stopping ccsd, SIGSEGV received.\n");
    err = EXIT_FAILURE;
    break;
  default:
    log_err("Stopping ccsd, unknown signal received.\n");
    err = EXIT_FAILURE;
  }

  EXIT("sig_handler");
  exit(err);
}


/**
 * daemonize
 *
 * This function will do the following:
 * - daemonize, if required
 * - set up the lockfile
 * - set up logging
 * - set up signal handlers
 * It will cause the program to exit if there is a failure.
 */
static void daemonize(void){
  int error=0;
  int pid;

  ENTER("daemonize");

  if(flags & FLAG_NODAEMON){
    log_dbg("Entering non-daemon mode.\n");
    if((error = create_lockfile(lockfile_location))){
      goto fail;
    }
    signal(SIGINT, &sig_handler);
    signal(SIGQUIT, &sig_handler);
    signal(SIGTERM, &sig_handler);
    signal(SIGSEGV, &sig_handler);
    if(flags & FLAG_VERBOSE){
      log_set_verbose();
    }
  } else {
    log_dbg("Entering daemon mode.\n");

    signal(SIGTERM, &parent_exit_handler);
    signal(SIGUSR1, &parent_exit_handler);

    pid = fork();

    if(pid < 0){
      fprintf(stderr, "Unable to fork().\n");
      error = pid;
      goto fail;
    }

    if(pid){
      while(parent_exit_code == -1){  /* parent awaits signal from child */
	sleep(5);
      }
      exit(parent_exit_code);
    }

    setsid();
    chdir("/");
    umask(0);

    close(0); close(1); close(2);
    open("/dev/null", O_RDONLY); /* reopen stdin */
    open("/dev/null", O_WRONLY); /* reopen stdout */
    open("/dev/null", O_WRONLY); /* reopen stderr */

    log_open("ccsd", LOG_PID, LOG_DAEMON);
    if(flags & FLAG_VERBOSE){
      log_set_verbose();
    }

    if((error = create_lockfile(lockfile_location))){
      kill(getppid(), SIGUSR1);
      goto fail;
    }

    kill(getppid(), SIGTERM);
    signal(SIGINT, &sig_handler);
    signal(SIGQUIT, &sig_handler);
    signal(SIGTERM, &sig_handler);
    signal(SIGSEGV, &sig_handler);
  }

 fail:
  EXIT("daemonize");

  if(error){
    exit(EXIT_FAILURE);
  }
}


/**
 * print_start_msg
 *
 */
static void print_start_msg(char *msg){
  log_msg("Starting ccsd %s:\n", CCS_RELEASE_NAME);
  log_msg(" Built: "__DATE__" "__TIME__"\n");
  log_msg(" %s\n", REDHAT_COPYRIGHT);
  if(msg){
    log_msg("%s\n", msg);
  }
}


static int join_group(int sfd, int loopback, int port){
  int error = 0;
  char *addr_string;
  struct sockaddr_storage addr;
  struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
  struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
 
  ENTER("join_group");
  
  if(IPv6){
    if(!multicast_address || !strcmp("default", multicast_address)){
      addr_string = "ff02::3:1";
    } else {
      addr_string = multicast_address;
    }
    inet_pton(AF_INET6, addr_string, &(addr6->sin6_addr));
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(port);
  } else {
    if(!strcmp("default", multicast_address)){
      addr_string = "224.0.2.5";
    } else {
      addr_string = multicast_address;
    }
    inet_pton(AF_INET, addr_string, &(addr4->sin_addr));
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
  }

  if(addr.ss_family == AF_INET){
    struct ip_mreq mreq;

    mreq.imr_multiaddr.s_addr = addr4->sin_addr.s_addr;
    mreq.imr_interface.s_addr = INADDR_ANY;

    if(setsockopt(sfd, IPPROTO_IP, IP_MULTICAST_LOOP,
		  &loopback, sizeof(loopback)) < 0){
      log_err("Unable to %s loopback.\n", loopback?"SET":"UNSET");
      error = -errno;
      goto fail;
    }
    if(setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
		  (const void *)&mreq, sizeof(mreq)) < 0){
      log_err("Unable to add to membership.\n");
      error = -errno;
      goto fail;
    }
  } else if(addr.ss_family == AF_INET6){
    struct ipv6_mreq mreq;

    memcpy(&mreq.ipv6mr_multiaddr, &(addr6->sin6_addr), sizeof(struct in6_addr));

    mreq.ipv6mr_interface = 0;

    if(setsockopt(sfd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
		  &loopback, sizeof(loopback)) < 0){
      log_err("Unable to %s loopback.\n", loopback?"SET":"UNSET");
      error = -errno;
      goto fail;
    }
    if(setsockopt(sfd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
		  (const void *)&mreq, sizeof(mreq)) < 0){
      log_err("Unable to add to membership: %s\n", strerror(errno));
      error = -errno;
      goto fail;
    }
  } else {
    log_err("Unknown address family.\n");
    error = -EINVAL;
  }
 fail:
  EXIT("join_group");
  return 0;
}
