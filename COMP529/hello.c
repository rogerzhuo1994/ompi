#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>



#define BUFFERSIZE 1000

static void fill(int seed, int* buffer) {
	srand(seed);
	for (int i = 0; i < BUFFERSIZE; i++)
//		buffer[i] = rand();
		buffer[i] = i;
}
static void validate(int seed, int* buffer) {
	srand(seed);
	for (int i = 0; i < BUFFERSIZE; i++) {
//		int expected = rand();
		int expected = i;
		if (buffer[i] != expected) {
			printf("Buffer data validation error. For seed %d, at position %d, buffer contained %d but %d was expected.\n", seed, i, buffer[i], expected);
			abort();
		}
	}
}
int main(int argc, char** argv) {
	MPI_Init(&argc, &argv);
	int rank, size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	printf("Hello world! I'm rank %d of %d.\n", rank, size);
	if (size != 2) {
		if (rank == 0)
			printf("This program can only run on two ranks.\n");
		MPI_Abort(MPI_COMM_WORLD, 1);
	}

	int buffer[BUFFERSIZE];
	// Rank 0 sends to 1 and 1 receives from 0
	if (rank % 2 == 0) {
		fill(12, buffer);
		MPI_Send(buffer, BUFFERSIZE, MPI_INT, rank+1, 2, MPI_COMM_WORLD);
	}
	else {
		MPI_Recv(buffer, BUFFERSIZE, MPI_INT, rank-1, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		for (int i = 0; i < 20; i++) {
			printf("%d: %d\n", i, (int)buffer[i]);
		}
		validate(12, buffer);
	}
	printf("Hello 1c. I'm rank %d of %d.\n", rank, size);

	for (int i = 0; i < 1; i++) {
		int root = i % 2;
		if (rank == root)
			fill(i+100, buffer);
		MPI_Bcast(buffer, BUFFERSIZE, MPI_INT, root, MPI_COMM_WORLD);
		if (rank != root)
			validate(i+100, buffer);
	}

	printf("Hello 1c. I'm rank %d of %d. I finished Bcast\n", rank, size);
	
	MPI_Finalize();
}
