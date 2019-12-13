#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>


#define HUGEBUFFERSIZE (10*1024/8*1024*1024) // 10 GB
#define LARGEBUFFERSIZE (1000*1000)  // approx 8 MB
#define SMALLBUFFERSIZE 25 // approx 200 B

#define RUNTIME 1.0 // seconds
#define PER_MESSAGE_OVERHEAD 0.0001 // seconds. No idea if this is right
#define BANDWIDTH 1.25e9 // Bytes/sec = 10 Gbps
int buffersize = 0;
bool mix_message_sizes = false;
bool synchronize = false;
int sender = 0;
bool rotate_sender = false;
int nlate = 0;
int nsubcomm  = 1;

static inline unsigned long nextrand() {
	unsigned long toreturn = rand();
	toreturn <<= 32;
	toreturn |= rand();
	return toreturn;
}
static void fill(int seed, unsigned long* buffer, int message_size) {
	srand(seed);
	for (int i = 0; i < message_size; i++)
		buffer[i] = nextrand();
}
static void validate(int seed, unsigned long* buffer, int message_size) {
	srand(seed);
	for (int i = 0; i < message_size; i++) {
		unsigned long expected = nextrand();
		if (buffer[i] != expected) {
			printf("Buffer data validation error. For seed %d, at position %d, buffer contained %lu but %lu was expected.\n", seed, i, buffer[i], expected);
			abort();
		}
	}
}

volatile double k;
static void stall() {
	double t = 1.0;
	for (int i = 0; i < 500000000; i++) {
		t = t/2+1;
	}
	k += t;
}
static void usage(const char* step) {
	fprintf(stderr, "Error parsing %s\n", step);
	fprintf(stderr, "Usage: ./grade -z {small, large, huge, mix} -y {yes, no} -s {0-3 | mix} -l {0-3} -b {1-3}\n");
	fprintf(stderr, "\t -z: Message size. small=200B, large = ~8 MB, huge = 10 GB\n");
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
				} else if (strcmp(optarg, "huge") == 0) {
					buffersize = HUGEBUFFERSIZE;
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
		printf("Running with buffer %lu bytes (mixed = %d). Synchronization: %d, sender: %d (rotating = %d), nlate: %d, subcomm: %d\n", buffersize*sizeof(unsigned long), (int) mix_message_sizes, (int) synchronize, sender, (int)rotate_sender, nlate, nsubcomm);
	}

	unsigned long* buffer = malloc(buffersize * sizeof(unsigned long));
	int nrounds = (int) (RUNTIME / (PER_MESSAGE_OVERHEAD + (.05 * (nlate > 0)) + sizeof(unsigned long) * buffersize / BANDWIDTH ));
	if (nrounds <= 0)
		nrounds = 1;
	if (rank == 0)
		printf("Running %d rounds\n", nrounds);

	MPI_Comm bcast_comm;
	if (nsubcomm == 1)
		bcast_comm = MPI_COMM_WORLD;
	else {
		int color = rank % nsubcomm;
		int ranks_in_my_group = size / nsubcomm;
		if (color < size % nsubcomm)
			ranks_in_my_group++;

		int* ranks = malloc(ranks_in_my_group * sizeof(int));
		for (int i = 0; i < ranks_in_my_group; i++)
			ranks[i] = color + nsubcomm * i;

		MPI_Group world_g;
		MPI_Comm_group(MPI_COMM_WORLD, &world_g);

		MPI_Group subcomm_g;
		MPI_Group_incl(world_g, ranks_in_my_group, ranks, &subcomm_g);
		MPI_Comm_create_group(MPI_COMM_WORLD, subcomm_g, 0, &bcast_comm);

		free(ranks);
		MPI_Group_free(&world_g);
		MPI_Group_free(&subcomm_g);

		// Bug in Portals4 prevents this line from working:
		// MPI_Comm_split(MPI_COMM_WORLD, rank % nsubcomm, rank, &bcast_comm);
	}
	int communicator_num = rank % nsubcomm;

	int subcomm_rank, subcomm_size;
	MPI_Comm_rank(bcast_comm, &subcomm_rank);
	MPI_Comm_size(bcast_comm, &subcomm_size);

	
	size_t bytes = 0;
	double start = MPI_Wtime();
	double end = 0;

	for (int i = 0; i < nrounds; i++) {

//		printf("Total %d round, current is %d\n", nrounds, i);

		if (subcomm_rank == 0) {
			printf("%d of %d\r", i, nrounds);
			fflush(stdout);
		}
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
			fill(i+100*communicator_num, buffer, message_size);
		if (nrounds == 1)
			start = MPI_Wtime(); // Don't include time to fill

		MPI_Bcast(buffer, message_size, MPI_UNSIGNED_LONG, root, bcast_comm);
		if (nrounds == 1)
			end = MPI_Wtime();
		if (subcomm_rank != root)
			validate(i+100*communicator_num, buffer, message_size);

		bytes += message_size * sizeof(unsigned long);
	}
	if (nrounds > 1)
		end = MPI_Wtime();
	if (rank == 0)
		printf("Transferred %zd bytes in %f seconds: %f Gbps\n", bytes, end - start, bytes/(end-start) * 8 * 1e-9);
	MPI_Finalize();
}
