#ifndef __service_c__
#define __service_c__

#define _BSD_SOURCE
#include "core.h"
#include "service.h"

ServiceMessage smessage_null = {{0xaaaaaaaaL, 0, 0, 0, 0, 0, 0, 0}, NULL, NULL};

// Neighbor's message receiving thread. This thread reads the messages from the neighbor and builds the ServiceMessage
// representation of them. Every neighbor has a dedicated thread. Monitoring every neighbor was better than polling all at once.
// This architecture will not force iterating over all the neighbors' connections everytime. Instead only the neighbor's thread which
// receives the message will handle.

// The read message is handed over to the service handled through pipes.
void *thread_read_handler(void *args){
	struct trhargs *trh = ((struct trhargs *)args);

	Config *config = trh->config;
	Neighbor *nb = trh->nb;
	int thread_write = trh->thread_write;

	ServiceMessage smessage;

	fd_set read_set;
	struct timeval timeout;

	while(1){
		smessage = smessage_null;
	
		// Read 	header -> clock -> data
		if(read(nb->in_fd, &smessage.header, sizeof(ServiceHeader)) <=0){
			// perror("read from ReaderThread\n");
			printf("(1)Illegal message generated by neighbor#%d! \n", nb->id);
			continue;
		} 
		if(smessage.header.syncbits != 0xaaaaaaaa){
			printf("(2)Illegal message generated by neighbor#%d! \n", nb->id);
			continue;
		}


		if(smessage.header.clock_size>0){
			smessage.clock = (int*)malloc(smessage.header.clock_size);
			memset(smessage.clock, 0, smessage.header.clock_size);
		}
		if(smessage.header.data_size>0){
			smessage.data = (void*)malloc(smessage.header.data_size);
			memset(smessage.data, 0, smessage.header.data_size);
			printf("Service node#%d received message from node#%d: %s\n", config->node.id, nb->id, smessage.data);
		}

		if(smessage.header.clock_size>0) read(nb->in_fd, smessage.clock, smessage.header.clock_size);
		if(smessage.header.data_size>0) read(nb->in_fd, smessage.data, smessage.header.data_size);

		
	
		write(thread_write, &smessage, sizeof(ServiceMessage));
	}
}

// Alarm for regular snapshot algorithm running
void *snapshot_alarm(void *args){
	Config *config = ((struct saargs*)args)->config;
	int *flag_snapshots_session_active = ((struct saargs*)args)->flag_snapshots_session_active;
	usleep(1000 * config->SnapshotDelay);
	*flag_snapshots_session_active = 1;
}

// This function is the service handler thread which runs as long as the termination is not detected.
// This thread takes the configuration and launcher neighbor handlers. Neighbor handlers read messages and send them to this 
// thread. In this thread, the messages are handled according to their type.

// This thread takes care of timestamping, and managing snapshots algorithm.
void* run_service(void *args){
	Config* config = ((struct rsargs*)args)->config;
	int app_read_fd = ((struct rsargs*)args)->pipe_fd[0];
	int app_write_fd = ((struct rsargs*)args)->pipe_fd[1];

	// Pipe for reader thread communication
	int thread_rw[2];
	pipe(thread_rw);
	int thread_read_fd = thread_rw[0];		// read from thread(s)
	int thread_write_fd = thread_rw[1];		// write to thread(s) - never used

	config->app_read_fd = app_read_fd;
	config->app_write_fd = app_write_fd;
	config->thread_read_fd = thread_read_fd;
	config->thread_write_fd = thread_write_fd;

	// Select operator needs this value
	const int nfds = thread_read_fd > app_read_fd ? thread_read_fd + 1 : app_read_fd + 1;

	// Service clock initialized to all zeros
	int service_clock[config->N];
	memset(&service_clock, 0, sizeof(int)*config->N);

	// Spawn threads for neighbors...
	pthread_t trh_tids[config->node.neighbors_count];
	struct trhargs trh_args[config->node.neighbors_count];

	for(int i = 0; i < config->node.neighbors_count; i++){
		trh_args[i].config = config;
		trh_args[i].nb = &config->node.neighbors[i];
		trh_args[i].thread_write = thread_write_fd;

		pthread_create(&trh_tids[i], NULL, thread_read_handler, &trh_args[i]);
	}

	// Select operator variables - thread/app pipe fds
	fd_set read_set;
	struct timeval timeout;

	// Variables for run loop
	ServiceMessage 		smessage;
	ApplicationHeader	aheader;

	int buffer_size = 128*sizeof(char);
	char *s_data_buffer_pointer = (char*)malloc(buffer_size);

	const struct saargs sargs = {config, &config->flag_snapshots_session_active};

	// Send Status query to application layer
	aheader.src_id = config->node.id;
	aheader.dst_id = config->node.id;
	aheader.data_size = 0;
	aheader.m_type = A_STATE_QUERY;

	write(app_write_fd, &aheader, sizeof(ApplicationHeader));

	config->flag_snapshots_session_active = 0;
	config->flag_snapshots_alarm_active = 0;
	config->flag_markers_sent = 0;

	while(1){
		// Start snapshots alarm
		if(config->node.id == 0 && config->flag_snapshots_session_active == 0 && config->flag_snapshots_alarm_active == 0){
			pthread_t alarm_t;
			config->flag_snapshots_alarm_active = 1;
			// printf("Starting alarm\n");
			pthread_create(&alarm_t, NULL, snapshot_alarm, (void*)&sargs);
		}

		if(config->node.id == 0){
			if(config->flag_snapshots_session_active == 1 && config->flag_markers_sent == 0){
				config->flag_markers_sent = 1;
				reset_snapshots_control_variables(config);

				memset(config->snapshots_session_children, 0, sizeof(int) * config->N);
				memcpy(config->snapshots_clock, config->clock, sizeof(int) * config->N);

				config->snapshots_session_children_count = 0;
				config->snapshots_session_ccast_count = 0;

				// Send markers to neighbors
				ServiceMessage marker = {{0xaaaaaaaaL, M_MARKER, config->node.id, 0, 0, 0, ++(config->snapshot_no), config->snapshots_session_spark}, NULL, NULL};

				printf("SNAPSHOTS session(%d) started on node#%d\n", config->snapshot_no, config->node.id);
				for(int i = 0; i < config->node.neighbors_count; i++){
					Neighbor *nb = &config->node.neighbors[i];

					printf( "Service     on node#%d sent (INIT)MARKER  to node#%d:          \t\tSnapshot(%d) | ",
					 config->node.id,
					 nb->id,
					 marker.header.snap_no
					);
					dispclock(config->clock, config->N);
					marker.header.dst_id = nb->id;
					write(nb->out_fd, &marker.header, sizeof(ServiceHeader));
				}
				
			}
		}
		

		// Reset Select operator variables
		FD_ZERO(&read_set);
		FD_SET(thread_read_fd, &read_set);
		FD_SET(app_read_fd, &read_set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 1000 * config->SnapshotDelay;		// Select operator on pipes will wait as long as the SnapshotDelay 

		// Handle incoming messages
		select(nfds, &read_set, NULL, NULL, &timeout);
		smessage = smessage_null;
		if(FD_ISSET(thread_read_fd, &read_set)){			// Content came from ReaderThread
			// Read the pre-allocated message
			read(thread_read_fd, &smessage, sizeof(ServiceMessage));

			// Handle the cases based on message type
			switch(smessage.header.m_type){
				case M_APPLICATION:{	// A normal application message was read by ReaderThread
					handle_app_recv_message(config, smessage);
					break;
				}
				case M_MARKER:{	// A snapshot process starting marker has arrived
					manage_marker(config, smessage);
					break;
				}
				case M_MARKER_ACK:{		// If ACK/NACK is received from a neighbor as a reply for MARKERs
					manage_ack(config, smessage);
					break;
				}
				case M_MARKER_NACK:{
					manage_nack(config, smessage);
					break;
				}
				case M_CCAST:{
					manage_ccast(config, smessage);
					break;
				}
				case M_MARKER_TERM:{	// If a termination marker is sent...
					manage_term(config, smessage);
					break;
				}
			}
		}

		// If all the neighbors have responded, - aggregate results if it is node#0
		// Other nodes send their own snapshot through converge cast.
		if(config->flag_snapshots_session_active == 1){
				int monitor = 0;
				for( int i = 0; i < config->node.neighbors_count; i++){
					Neighbor *nb = &config->node.neighbors[i];
					monitor += config->snapshots_session_monitor_punch_card[nb->id];
				}
		
				if(monitor == config->node.neighbors_count){
					if(config->node.id == 0){
						if(config->snapshots_session_ccast_count == config->N-1){
		
							int diagonal[config->N];
		
							for(int i = 0; i < config->N; i++){
								config->snapshots_accumulated_timestamps[ config->node.id * config->N + i] = config->snapshots_clock[i];
							}
		
							// node0_print_snapshot_accum(config);
		
							for(int i = -1; i < config->N; i++){
								for(int j = -1; j < config->N; j++){
									if(i == -1){
										printf(" [%4d]|", j);
									}else{
										if(j == -1)printf(" [%4d]|", i);
										else{
											printf("  %4d  ", ((int*)config->snapshots_accumulated_timestamps)[i * config->N + j] );
											if( i == j)diagonal[i] = ((int*)config->snapshots_accumulated_timestamps)[i * config->N + j];
										}
									}
								}
								printf("\n\n");
							}
		
							int consistency = 1;
		
							for( int i = 0; i < config->N; i++){
								for( int j = 0; j < config->N; j++){
									consistency = consistency & (diagonal[j] >= ((int*)config->snapshots_accumulated_timestamps)[i * config->N + j]);
								}
							}
		
							if(consistency == 1){
								printf("A consistent snapshot has been determined\n");
							}
		
							save_local_snapshot(config);
							
							printf("config->snapshots_session_spark = %d\n",config->snapshots_session_spark );
							if(config->snapshots_session_spark == 0){
								printf("------------------TERMINATION determined-----------------------\n");
								// Send markers to neighbors
								ServiceMessage term = {{0xaaaaaaaaL, M_MARKER_TERM, config->node.id, 0, 0, 0, ++(config->snapshot_no), config->snapshots_session_spark}, NULL, NULL};
		
								for(int i = 0; i < config->node.neighbors_count; i++){
									Neighbor *nb = &config->node.neighbors[i];
		
									printf( "Service     on node#%d sent (TERM)MARKER  to node#%d:          \t\tSnapshot(%d) | ",
									 config->node.id,
									 nb->id,
									 term.header.snap_no
									);
									dispclock(config->clock, config->N);
									term.header.dst_id = nb->id;
									write(nb->out_fd, &term.header, sizeof(ServiceHeader));
								}
		
								sleep(2);
								exit(0);
							}
		
							config->snapshots_session_spark = 0;
		
							config->flag_snapshots_session_active = 0;
							config->flag_markers_sent = 0;
							config->flag_snapshots_alarm_active = 0;
							config->snapshots_session_ccast_count = 0;
							config->snapshots_session_children_count = 0;
							// save_local_snapshot(config);
		
							reset_snapshots_control_variables(config);
							printf("SNAPSHOTS session(%d) ended on node#%d\n", config->snapshot_no, config->node.id);
							// Snapshots are completed
						}
					}else{
						// printf("config->snapshots_session_ccast_count(%d) == config->snapshots_session_children_count(%d)\n",
						// 	config->snapshots_session_ccast_count, config->snapshots_session_children_count);
						if(config->snapshots_session_ccast_count == config->snapshots_session_children_count){
							printf("Node#%d sending it's OWN CCAST to parent node#%d\n", config->node.id, config->snapshots_session_parent_id);
		
							Neighbor *nb = get_neighbor(config, config->snapshots_session_parent_id);
		
							// printf("config->snapshots_session_spark = %d\n",config->snapshots_session_spark);
		
							ServiceMessage ccast = {{0xaaaaaaaaL,
							 M_CCAST,
							 config->node.id,
							 nb->id,
							 0,
							  sizeof(int) * config->N, config->snapshot_no, config->snapshots_session_spark}, NULL, NULL};
		
							
							write(nb->out_fd, &ccast.header, sizeof(ServiceHeader));
							fsync(nb->out_fd);
							write(nb->out_fd, config->snapshots_clock, sizeof(int) * config->N);
							fsync(nb->out_fd);
		
		
							config->flag_snapshots_session_active = 0;
							config->flag_markers_sent = 0;
							config->flag_snapshots_alarm_active = 0;
							config->snapshots_session_ccast_count = 0;
							config->snapshots_session_children_count = 0;
							save_local_snapshot(config);
		
		
							reset_snapshots_control_variables(config);
							printf("SNAPSHOTS session(%d) ended on node#%d - CHILDREN - ", config->snapshot_no, config->node.id);
							for(int i = 0; i < config-> N; i++)if(config->snapshots_session_children[i]==1)printf("%d ", i);
								printf("\n");
		
							config->snapshots_session_spark = 0;
							// seek_application_state(config);
							// Snapshots are completed
						}
					}
		
				}
		}

		// Handle own application's outgoing messages
		if(FD_ISSET(app_read_fd, &read_set)){ 		// Content cam from app wanting to send a message
			read(app_read_fd, &aheader, sizeof(ApplicationHeader));

			switch(aheader.m_type){
				case A_STATE_REPLY:{
					config->snapshots_session_spark = aheader.state;
					break;
				}
				case A_MESSAGE:{
					config->snapshots_session_spark = 1;
					handle_app_send_message(config, aheader);
					break;
				}
			}
		}
		// seek_application_state(config);
	}

	for(int i = 0; i < config->node.neighbors_count; i++)
		pthread_cancel(trh_tids[i]);
}

// This function connects service layer to application layer through pipe filedescriptors
int start_service(Config* config, int fd[2]){
	static int service_active = 0;
	usleep(config->N * 100000);
	if(service_active)return(0);

	struct rsargs *args = (struct rsargs*)malloc(sizeof(struct rsargs));

	int app_rw[2];
	int service_rw[2];

	if(pipe(app_rw) < 0){
		perror(STR(__LINE__) "pipe");
		exit(-1);
	}

	if(pipe(service_rw) < 0){
		perror(STR(__LINE__) "pipe");
		exit(-1);
	}
	
	args->config = config;
	args->pipe_fd[0] = app_rw[0]; // Service read end = Application write end
	args->pipe_fd[1] = service_rw[1]; // Service write end = Application read end

	fd[0] = service_rw[0]; //Application read end = Service write end
	fd[1] = app_rw[1]; // Application write end = Service read end

	pthread_t service_t;

	printf("Initiating service on node#%d\n", config->node.id);
	pthread_create(&service_t, NULL, run_service, args);

	service_active = -1;
	return(0);

}
#endif