#include <mpi.h>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <random>
#include <iostream>
#include <functional>
#include <unistd.h>

#define RED "\033[41m"
#define GRN "\033[42m"
#define YLW "\033[43m"
#define NLC "\033[0m\n"

using namespace std;

enum Tag { REQ, OK, END };
struct Msg { Tag tag; int time; int queue; };

// maksymalna liczba studentów biorących udział w zawodach
const int max_students = 20;
// liczba arbitrów
const int num_arbiters = 4;
// czas trwania zawodów
const double max_time = 2;
// czas snu między yieldami
const int sleep_time = 2000;

// true, gdy recv oczekuje na wiadomość
bool recv_await = false;
// odebrana wiadomość
Msg msg;
// maksymalny globalny rank (stała po inicjalizacji)
int max_rank;
// lokalny rank (stała po inicjalizacji)
int my_rank;
// lokalny zegar Lamporta
int my_time = 0;

bool executing[max_students+1] = {false};
bool requesting[max_students+1] = {false};
vector<int> deferred[max_students+1];
int accepted[max_students+1] = {0};
int exited = 0;

#define debug(format, ...) fprintf(stderr, "%4d P%d " format, my_time, my_rank, ##__VA_ARGS__)

void tick(int new_time = 0) {
	my_time = max(my_time, new_time) + 1;
}

MPI_Request request = {0};
MPI_Status status = {0};

bool recv(int *src, Msg *msg) {

	if (!recv_await) {
		MPI_Irecv(msg, sizeof(Msg), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &request);
		recv_await = true;
	}

	int completed;
	MPI_Test(&request, &completed, &status);
	if (!completed) return false;

	*src = status.MPI_SOURCE;
	recv_await = false;
	return true;
}

void send(int dst, Tag tag, int queue) {
	Msg msg {.tag = tag, .time = my_time, .queue = queue};
	MPI_Send(&msg, sizeof(Msg), MPI_BYTE, dst, 0, MPI_COMM_WORLD);
}

void append(vector<int> &xs, int x) {
	xs.push_back(x);
}

vector<int> range(int start, int end) {
    vector<int> res;
    for (int i = start; i < end; i++)
        res.push_back(i);
    return res;
}

void respond(int req_time = -1) {
	int src;
	if (!recv(&src, &msg)) return;
	tick(msg.time);

	if (msg.tag == REQ) {
		bool lower_priority = msg.time == req_time ? src < my_rank : msg.time < req_time;
		if ((!requesting[msg.queue] && !executing[msg.queue]) || (requesting[msg.queue] && lower_priority)) {
			tick();
			send(src, OK, msg.queue);
		} else {
			append(deferred[msg.queue], src);
		}
	} else if (msg.tag == OK) {
		accepted[msg.queue] += 1;
	} else if (msg.tag == END) {
		exited += 1;
	}
}

void P(int queue_id, int capacity) {
	tick();
	int req_time = my_time;
	for (int rank = 0; rank < max_rank; rank++)
		if (rank != my_rank)
			send(rank, REQ, queue_id);

	requesting[queue_id] = true;
	accepted[queue_id] = 0;
	while (accepted[queue_id] < max_rank - capacity) {
		respond(req_time);
		usleep(sleep_time);
	}

	//debug("wykonuje %d (%d >= %d)\n", queue_id, accepted[queue_id], max_rank - capacity);
	requesting[queue_id] = false;
	executing[queue_id] = true;
}

void V(int queue_id) {
	tick();
	for (int rank : deferred[queue_id])
		send(rank, OK, queue_id);
	deferred[queue_id].clear();
	executing[queue_id] = false;
}

void JOIN() {
	exited += 1;

	tick();
	for (int rank : range(0, max_rank))
		if (rank != my_rank)
			send(rank, END, -1);

	while (exited < max_rank)
		respond();
}

int main(int argc, char **argv) {
	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &max_rank);

	if (argc < 2) {
		fprintf(stderr, "Użycie: %s liczba_arbitrów\n", argv[0]);
		MPI_Finalize();
		return 1;
	}

	int num_arbiters = atoi(argv[1]);
	if (num_arbiters < 1) {
		fprintf(stderr, "Liczba arbitrów musi być większa niż 0.\n");
		MPI_Finalize();
		return 1;
	}

	debug("mój rank %d (maksymalny rank %d), liczba arbitrów %d\n", my_rank, max_rank, num_arbiters);

	srand(time(0));
	for (int iter = 1; true; iter++) {
		int num_students = rand() % (max_students - 1) + 2;
		exited = 0;

		debug("liczba studentów %d\n", num_students);
		debug(YLW "wejdzie na zawody" NLC);
		P(0, num_students);
		debug(GRN "wszedł na zawody" NLC);
		int start = clock();
		while (double(clock() - start) / CLOCKS_PER_SEC < max_time) {
			respond();
		}
		V(0);
		debug(RED "wyszedł z zawodów" NLC);

		JOIN();
		debug(RED "-- iteracja %d ----------------" NLC, iter);
	}

	MPI_Finalize();
	return 0;
}
