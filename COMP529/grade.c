#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>


#define LARGEBUFFERSIZE (1000*1000)  // approx 4 GB
#define SMALLBUFFERSIZE 50 // approx 200 B

#define RUNTIME 10.0 // seconds
#define PER_MESSAGE_OVERHEAD 0.0001 // seconds. No idea if this is right
#define BANDWIDTH 1.25e9 // Bytes/sec = 10 Gbps
int buffersize = 0;
bool mix_message_sizes = false;
bool synchronize = false;
int sender = 0;
bool rotate_sender = false;
int nlate = 0;
int nsubcomm  = 1;

static void fill(int seed, int* buffer) {
	srand(seed);
	for (int i = 0; i < buffersize; i++)
		buffer[i] = rand();
}
static void validate(int seed, int* buffer) {
	srand(seed);
	for (int i = 0; i < buffersize; i++) {
		int expected = rand();
		if (buffer[i] != expected) {
			printf("Buffer data validation error. For seed %d, at position %d, buffer contained %d but %d was expected.\n", seed, i, buffer[i], expected);
			abort();
		}
	}
}

volatile double k;
static void stall() {
	double t = 1.0;
	for (int i = 0; i < 1000000000; i++) {
		t = t/2+1;
	}
	k += t;
}
static void usage(const char* step) {
	fprintf(stderr, "Error parsing %s\n", step);
	fprintf(stderr, "Usage: ./grade -z {small, large, mix} -y {yes, no} -s {0-3 | mix} -l {0-3} -b {1-3}\n");
	fprintf(stderr, "\t -z: Message size. small=200B, large = ~8 GB\n");
	fprintf(stderr, "\t -y: Syncronize between each broadcast\n");
	fprintf(stderr, "\t -s: Fix a sender or rotate between senders\n");
	fprintf(stderr, "\t -l: Number of late arrivers to the broadcast\n");
	fprintf(stderr, "\t -b: Number of subcommunicators\n");
	MPI_Abort(MPI_COMM_WORLD, -1);
}

int main(int argc, char** argv) {
	MPI_Init(&argc, &argv);
	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	printf("Hello world! I'm rank %d of %d.\n", rank, size);

	int opt;
	while ((opt = getopt(argc, argv, "z:y:s:l:b:")) != -1) {
		switch (opt) {
			case 'z':
				// Message size
				if (strcmp(optarg, "small") == 0) {
					buffersize = SMALLBUFFERSIZE;
				} else if (strcmp(optarg, "large") == 0) {
					buffersize = LARGEBUFFERSIZE;
				} else if (strcmp(optarg, "mix") == 0) {
					buffersize = LARGEBUFFERSIZE;
					mix_message_sizes = true;
				} else 
					usage("size");
				break;
			case 'y':
				// Sync
				if (strcmp(optarg, "yes") == 0) {
					synchronize = true;
				} else if (strcmp(optarg, "no") == 0) {
					synchronize = false;
				} else
					usage("sync");
				break;
			case 's':
				// sender
				if (strcmp(optarg, "mix") == 0) {
					rotate_sender = true;
				} else {
					sender = atoi(optarg);
				}
				break;
			case 'l':
				// Late
				nlate = atoi(optarg);
				break;
			case 'b':
				// Subcommunicators
				nsubcomm = atoi(optarg);
				break;
			default:
				usage("arg");
				break;
		}
	}
	if (rank == 1) {
		printf("Running with buffer %lu bytes (mixed = %d). Synchronization: %d, sender: %d (rotating = %d), nlate: %d, subcomm: %d\n", buffersize*sizeof(int), (int) mix_message_sizes, (int) synchronize, sender, (int)rotate_sender, nlate, nsubcomm);
	}

	int* buffer = malloc(buffersize * sizeof(int));
	int nrounds = (int) (RUNTIME / (PER_MESSAGE_OVERHEAD + sizeof(int) * buffersize / BANDWIDTH ));
	buffer[0] = 0;
	if (rank == 0)
		printf("Running %d rounds\n", nrounds);

	MPI_Comm bcast_comm;
	if (nsubcomm == 1)
		bcast_comm = MPI_COMM_WORLD;
	else
		MPI_Comm_split(MPI_COMM_WORLD, rank % nsubcomm, rank, &bcast_comm);
	int communicator_num = rank % nsubcomm;

	int subcomm_rank, subcomm_size;
	MPI_Comm_rank(bcast_comm, &subcomm_rank);
	MPI_Comm_size(bcast_comm, &subcomm_size);

	
	size_t bytes = 0;
	double start = MPI_Wtime();

	for (int i = 0; i < nrounds; i++) {
		if (subcomm_rank == 0)
			printf("%d of %d\n", i, nrounds);
		if (synchronize) {
			MPI_Barrier(bcast_comm);
		}
		int root = sender;
		if (rotate_sender)
			root = rand() % subcomm_size;

		int message_size = buffersize;
		if (mix_message_sizes)
			message_size = (int) (rand() / (1.0 * RAND_MAX) * (LARGEBUFFERSIZE - SMALLBUFFERSIZE)) + SMALLBUFFERSIZE;

		if ((subcomm_rank - root + subcomm_size) % subcomm_size < nlate)
			stall();

		if (subcomm_rank == root)
			fill(i+100*communicator_num, buffer);
		MPI_Bcast(buffer, message_size, MPI_INT, root, bcast_comm);
		if (subcomm_rank != root)
			validate(i+100*communicator_num, buffer);

		bytes += message_size * sizeof(int);
	}
	double end = MPI_Wtime();
	if (rank == 0)
		printf("Transferred %zd bytes in %f seconds: %f Gbps\n", bytes, end - start, bytes/(end-start) * 8 * 1e-9);
	MPI_Finalize();
}
