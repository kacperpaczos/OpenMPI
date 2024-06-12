#include <mpi.h>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <random>
#include <iostream>
#include <unistd.h>

#define RED "\033[41m"
#define GRN "\033[42m"
#define YLW "\033[43m"
#define BLU "\033[44m"
#define MAG "\033[45m"
#define CYN "\033[46m"
#define NLC "\033[0m\n"

using namespace std;

enum class MessageTag { REQUEST, APPROVE, TERMINATE, NUM_STUDENTS };
struct Message { MessageTag tag; int timestamp; int queue_id; int random_value; };

// Maksymalna liczba studentów biorących udział w zawodach
constexpr int MAX_STUDENTS = 20;
// Czas trwania zawodów w sekundach
constexpr double MAX_COMPETITION_TIME = 5.0;
// Czas snu między sprawdzeniami wiadomości (w mikrosekundach)
constexpr int SLEEP_DURATION = 2000;

enum class StudentState { NOT_PARTICIPATING, WANTS_TO_PARTICIPATE, HAS_ACCESS_TO_ARBITER, PARTICIPATING };
array<StudentState, MAX_STUDENTS + 1> student_states;

bool is_waiting_for_message = false;
Message received_message;
int max_rank;
int my_rank;
int local_time = 0;

array<bool, MAX_STUDENTS + 1> is_executing = {false};
array<bool, MAX_STUDENTS + 1> is_requesting = {false};
array<vector<int>, MAX_STUDENTS + 1> deferred_requests;
array<int, MAX_STUDENTS + 1> approvals_received = {0};
int processes_exited = 0;
int num_students;

#define debug(format, ...) fprintf(stderr, "%4d P%d " format, local_time, my_rank, ##__VA_ARGS__)

void update_time(int new_time = 0) {
    local_time = max(local_time, new_time) + 1;
}

MPI_Request mpi_request;
MPI_Status mpi_status;

bool receive_message(int *source, Message *msg) {
    if (!is_waiting_for_message) {
        MPI_Irecv(msg, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &mpi_request);
        is_waiting_for_message = true;
    }

    int completed;
    MPI_Test(&mpi_request, &completed, &mpi_status);
    if (!completed) return false;

    *source = mpi_status.MPI_SOURCE;
    is_waiting_for_message = false;
    return true;
}

void send_message(int destination, MessageTag tag, int queue_id, int random_value = 0) {
    Message msg {.tag = tag, .timestamp = local_time, .queue_id = queue_id, .random_value = random_value};
    MPI_Send(&msg, sizeof(Message), MPI_BYTE, destination, 0, MPI_COMM_WORLD);
}

void append_to_vector(vector<int> &vec, int value) {
    vec.push_back(value);
}

vector<int> create_range(int start, int end) {
    vector<int> result;
    for (int i = start; i < end; i++)
        result.push_back(i);
    return result;
}

void handle_message(int request_time = -1) {
    int source;
    if (!receive_message(&source, &received_message)) return;
    update_time(received_message.timestamp);

    if (received_message.tag == MessageTag::REQUEST) {
        bool has_lower_priority = received_message.timestamp == request_time ? source < my_rank : received_message.timestamp < request_time;
        if ((!is_requesting[received_message.queue_id] && !is_executing[received_message.queue_id]) || (is_requesting[received_message.queue_id] && has_lower_priority)) {
            update_time();
            send_message(source, MessageTag::APPROVE, received_message.queue_id);
        } else {
            append_to_vector(deferred_requests[received_message.queue_id], source);
        }
    } else if (received_message.tag == MessageTag::APPROVE) {
        approvals_received[received_message.queue_id] += 1;
    } else if (received_message.tag == MessageTag::TERMINATE) {
        processes_exited += 1;
    } else if (received_message.tag == MessageTag::NUM_STUDENTS) {
        num_students = received_message.random_value;
        debug("Otrzymano liczbę studentów: %d\n", num_students);
    }
}

void enter_critical_section(int queue_id, int capacity) {
    update_time();
    int request_time = local_time;
    for (int rank = 0; rank < max_rank; rank++)
        if (rank != my_rank)
            send_message(rank, MessageTag::REQUEST, queue_id);

    is_requesting[queue_id] = true;
    approvals_received[queue_id] = 0;
    while (approvals_received[queue_id] < max_rank - capacity) {
        handle_message(request_time);
        usleep(SLEEP_DURATION);
    }

    is_requesting[queue_id] = false;
    is_executing[queue_id] = true;
}

void leave_critical_section(int queue_id) {
    update_time();
    for (int rank : deferred_requests[queue_id])
        send_message(rank, MessageTag::APPROVE, queue_id);
    deferred_requests[queue_id].clear();
    is_executing[queue_id] = false;
}

void synchronize_processes() {
    processes_exited += 1;

    update_time();
    for (int rank : create_range(0, max_rank))
        if (rank != my_rank)
            send_message(rank, MessageTag::TERMINATE, -1);

    while (processes_exited < max_rank)
        handle_message();
}

void request_arbiter_access(int student_id) {
    update_time();
    int request_time = local_time;
    for (int rank = 0; rank < max_rank; rank++)
        if (rank != my_rank)
            send_message(rank, MessageTag::REQUEST, student_id);

    is_requesting[student_id] = true;
    approvals_received[student_id] = 0;
    while (approvals_received[student_id] < max_rank - 1) {  // Wszyscy muszą zaakceptować
        handle_message(request_time);
        usleep(SLEEP_DURATION);
    }

    is_requesting[student_id] = false;
    is_executing[student_id] = true;
    student_states[my_rank] = StudentState::PARTICIPATING;
    debug(CYN "zapisał się na zawody" NLC);
}

void release_arbiter(int student_id) {
    update_time();
    for (int rank : deferred_requests[student_id])
        send_message(rank, MessageTag::APPROVE, student_id);
    deferred_requests[student_id].clear();
    is_executing[student_id] = false;
    student_states[my_rank] = StudentState::NOT_PARTICIPATING;
    debug(MAG "nie bierze [już/jeszcze] udziału w zawodach" NLC);
}

void change_state_to_wants_to_participate(int student_id, std::mt19937 &generator) {
    std::uniform_int_distribution<int> distribution(3, 7); // Losowy czas między 3 a 7 sekund
    int delay = distribution(generator);
    debug(BLU "chce wziąć udział w zawodach, zajmie mu to %d sekund" NLC, delay);
    sleep(delay);
    student_states[student_id] = StudentState::WANTS_TO_PARTICIPATE;
    if (student_id == my_rank) {
        request_arbiter_access(student_id); // Ubieganie się o dostęp do arbitrów
    }
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

    // Używamy czasu i ranku procesu jako ziarna
    unsigned seed = time(NULL) + my_rank;
    std::mt19937 generator(seed);

    if (my_rank == 0) {
        std::uniform_int_distribution<int> student_distribution(2, MAX_STUDENTS);
        num_students = student_distribution(generator);
        debug("Liczba studentów: %d\n", num_students);
        for (int rank = 1; rank < max_rank; rank++) {
            send_message(rank, MessageTag::NUM_STUDENTS, -1, num_students);
        }
    } else {
        // Inne procesy czekają na wiadomość od root
        while (true) {
            handle_message();
            if (received_message.tag == MessageTag::NUM_STUDENTS) {
                break;
            }
            usleep(SLEEP_DURATION);
        }
    }

    for (int iter = 1; true; iter++) {
        processes_exited = 0;

        debug("liczba studentów %d\n", num_students);
        debug(YLW "wejdzie na zawody" NLC);
        for (int student_id = 0; student_id < MAX_STUDENTS; student_id++) {
            if (student_id == my_rank) { // Każdy proces odpowiada za jednego studenta
                change_state_to_wants_to_participate(student_id, generator);
            }
        }
        enter_critical_section(0, num_students);
        debug(GRN "wszedł na zawody" NLC);
        int start = clock();
        while (double(clock() - start) / CLOCKS_PER_SEC < MAX_COMPETITION_TIME) {
            handle_message();
        }
        leave_critical_section(0);
        debug(RED "wyszedł z zawodów" NLC);
        release_arbiter(my_rank);

        synchronize_processes();
        if (my_rank == 0) {
            debug(RED "-- iteracja %d ----------------" NLC, iter);
        }
    }

    MPI_Finalize();
    return 0;
}
