#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

// Maximum line length for input/output buffers
#define MAX_LINE 512
// Number of questions in the exam
#define NUM_EXAM_QUESTIONS 5
// Minimum time (in seconds) to consider an answer as not suspicious
#define MIN_ANSWER_TIME 5
// Server IP and port configuration
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080

// Flag to indicate if overall exam time is up (shared between threads)
volatile int examTimeUp = 0;
// Total allowed time for the entire exam (in seconds)
int overallExamTime = 300;

// Structure representing a single MCQ question
typedef struct {
    char question[MAX_LINE];
    char optionA[MAX_LINE];
    char optionB[MAX_LINE];
    char optionC[MAX_LINE];
    char optionD[MAX_LINE];
    char correct;      // Correct answer: 'A', 'B', 'C', or 'D'
    int difficulty;    // Difficulty level: 1 (easy), 2 (medium), 3 (hard)
} Question;

// Structure for storing a student's exam result
typedef struct {
    char roll[MAX_LINE];
    char name[MAX_LINE];
    int responseTimes[NUM_EXAM_QUESTIONS]; // Time taken for each answer
    int totalTime;                         // Total time spent in exam
    int correctAnswers;                    // Number of correct answers
    int totalQuestions;                    // Number of attempted questions
    int flagged;                           // 1 if suspicious, 0 otherwise
} ExamResult;

// Utility: Clear any remaining input in the stdin buffer
void clear_input_buffer() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

// Utility: Read password from terminal without echoing input
void getPassword(char *password, int size) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);           // Save current terminal settings
    newt = oldt;
    newt.c_lflag &= ~(ECHO);                  // Disable echo
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);  // Apply new settings

    if (fgets(password, size, stdin) != NULL) {
        password[strcspn(password, "\n")] = '\0'; // Remove newline
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);  // Restore old settings
    printf("\n");
}

// Reads user input with a timeout. Returns 1 if input was read, 0 if timed out or error.
int get_input_with_timeout(char *buf, int buf_size, int timeout_seconds) {
    fd_set set;
    struct timeval timeout;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;

    int rv = select(STDIN_FILENO + 1, &set, NULL, NULL, &timeout);
    if (rv == -1) {
        perror("📛 select");
        return 0;
    } else if (rv == 0) {
        // Timed out
        return 0;
    } else {
        // Input available
        if (fgets(buf, buf_size, stdin) == NULL) {
            return 0;
        }
        buf[strcspn(buf, "\n")] = '\0'; // Remove newline
        return 1;
    }
}

// Thread function: Waits for the overall exam time, then sets examTimeUp flag
void *overall_timer(void *arg) {
    sleep(overallExamTime); // Wait for the total exam duration
    examTimeUp = 1;         // Signal that time is up
    printf("\n⏰ *** Overall exam time is up! The exam will now end. ***\n");
    pthread_exit(NULL);
}

// Conducts the exam: presents questions, collects answers, times responses, and computes results
void conduct_exam(int sock, char *roll, char *name, Question *questions, int totalQuestions, int answerTimeout) {
    ExamResult result = {0}; // Initialize result structure
    strcpy(result.roll, roll);
    strcpy(result.name, name);

    double weightedScore = 0.0; // Score with difficulty weights
    int wrongCount = 0, attempted = 0;
    int isCheating = 0;         // Flag for suspicious activity
    time_t questionStartTime;
    int totalAnswerTime = 0;

    // Arrays for per-difficulty statistics
    int correctByDifficulty[4] = {0};
    int attemptedByDifficulty[4] = {0};
    int timeByDifficulty[4] = {0};
    char* diffNames[] = {"", "⭐ Easy", "⭐⭐ Medium", "⭐⭐⭐ Hard"};
    float diffWeights[] = {0, 1.0, 1.5, 2.0}; // Scoring weights per difficulty

    // Shuffle question indices for random order
    int indices[totalQuestions];
    for (int i = 0; i < totalQuestions; i++)
        indices[i] = i;
    srand(time(NULL));
    for (int i = totalQuestions - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }

    // Exam instructions
    printf("\n📝 Exam starting now. You will be shown %d questions.\n", totalQuestions);
    printf("⏱️  You have %d seconds per question.\n", answerTimeout);
    printf("⏳ Overall exam time: %d seconds.\n", overallExamTime);
    printf("💡 Question weights: Easy(x%.1f) Medium(x%.1f) Hard(x%.1f)\n",
           diffWeights[1], diffWeights[2], diffWeights[3]);
    printf("🚪 Enter 'e' at any time to exit the exam.\n\n");

    char answerBuf[20];
    for (int i = 0; i < totalQuestions; i++) {
        if (examTimeUp) {
            printf("\n⏰ Overall exam time has expired.\n");
            break;
        }

        Question *q = &questions[indices[i]];
        // Validate question data
        if (q->question[0] == '\0' || q->difficulty < 1 || q->difficulty > 3) {
            printf("📛 Invalid question %d, skipping\n", i+1);
            wrongCount++;
            attempted++;
            continue;
        }

        // Display question and options
        printf("\n--------------------------------------------------\n");
        printf("| 🔹 Q%-38d | %-12s |\n", i+1, diffNames[q->difficulty]);
        printf("--------------------------------------------------\n");
        printf("| %-47s |\n", q->question);
        printf("| 🅰️  %-45s |\n", q->optionA);
        printf("| 🅱️  %-45s |\n", q->optionB);
        printf("| ©️  %-45s |\n", q->optionC);
        printf("| 🅳  %-45s |\n", q->optionD);
        printf("--------------------------------------------------\n");

        printf("💭 Your answer (A/B/C/D or 'e' to exit): ");
        fflush(stdout);

        // Start timer for this question
        questionStartTime = time(NULL);
        int gotInput = get_input_with_timeout(answerBuf, sizeof(answerBuf), answerTimeout);
        int answerTime = time(NULL) - questionStartTime;
        totalAnswerTime += answerTime;
        timeByDifficulty[q->difficulty] += answerTime;
        result.responseTimes[i] = answerTime;

        clear_input_buffer();

        if (!gotInput) {
            // No answer provided in time
            printf("\n⏰ Time's up for this question! No answer provided.\n");
            wrongCount++;
            attempted++;
            attemptedByDifficulty[q->difficulty]++;
            printf("\n--------------------------\n\n");
            continue;
        }

        if (answerBuf[0] == 'e' || answerBuf[0] == 'E') {
            // User chose to exit exam
            printf("\n🚪 Exiting exam early...\n");
            break;
        }

        char userAns = toupper(answerBuf[0]);
        if (strchr("ABCD", userAns) == NULL) {
            // Invalid answer format
            printf("\n📛 Invalid answer! Treated as wrong.\n");
            wrongCount++;
            attempted++;
            attemptedByDifficulty[q->difficulty]++;
            continue;
        }

        attempted++;
        attemptedByDifficulty[q->difficulty]++;

        // Flag suspiciously fast answers
        if (answerTime < MIN_ANSWER_TIME) {
            printf("\n⚠️  Warning: You answered very quickly (%d seconds).\n", answerTime);
            isCheating = 1;
        }

        if (userAns == q->correct) {
            // Correct answer
            printf("✅ Correct! (+%.1f points)\n", diffWeights[q->difficulty]);
            weightedScore += diffWeights[q->difficulty];
            correctByDifficulty[q->difficulty]++;
        } else {
            // Wrong answer
            printf("❌ Wrong! Correct answer: %c\n", q->correct);
            wrongCount++;
        }
        printf("\n--------------------------\n\n");
    }

    // Additional cheating check: average answer time too low
    if (attempted > 0 && (totalAnswerTime/attempted) < MIN_ANSWER_TIME) {
        isCheating = 1;
    }

    int correctCount = attempted - wrongCount;
    double accuracy = (attempted > 0) ? ((double)correctCount / attempted) * 100 : 0;

    // Print summary and detailed stats
    printf("\n***********************************************************************\n");
    printf("*                         📊 DETAILED RESULTS                        *\n");
    printf("***********************************************************************\n");
    printf("| %-25s: %10.2f (Max: %.1f)                   |\n", "🎯 Weighted Score", weightedScore,
          totalQuestions * diffWeights[3]);
    printf("| %-25s: %10.2f%%                                   |\n", "📈 Overall Accuracy", accuracy);

    printf("\n--------------------------------------------------------\n");
    printf("| %-12s | %-8s | %-8s | %-8s | %-8s |\n", "Difficulty", "Correct", "Attempted", "Accuracy", "Avg Time");
    printf("--------------------------------------------------------\n");

    for (int i = 1; i <= 3; i++) {
        if (attemptedByDifficulty[i] > 0) {
            float diffAccuracy = (float)correctByDifficulty[i]/attemptedByDifficulty[i]*100;
            float avgTime = (float)timeByDifficulty[i]/attemptedByDifficulty[i];

            printf("| %-12s | %-8d | %-8d | %-7.1f%% | %-7.1fs |\n",
                  diffNames[i],
                  correctByDifficulty[i],
                  attemptedByDifficulty[i],
                  diffAccuracy,
                  avgTime);
        }
    }

    printf("--------------------------------------------------------\n");
    printf("| %-25s: %10d                                     |\n", "📝 Total Attempted", attempted);
    printf("***********************************************************************\n\n");

    // Fill result structure for sending to server
    result.correctAnswers = correctCount;
    result.totalQuestions = attempted;
    result.totalTime = totalAnswerTime;
    result.flagged = isCheating;

    // Send result to server
    if (send(sock, &result, sizeof(ExamResult), 0) < 0) {
        perror("📛 Error sending exam result");
    } else {
        printf("📤 Sent exam result to server\n");
    }
}

int main() {
    printf("\n\n✨✨✨ Welcome to ExamSys - Student Client ✨✨✨\n\n");

    // Create socket for connection to server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("📛 Error creating socket");
        exit(EXIT_FAILURE);
    }

    // Prepare server address structure
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    // Connect to the server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("📛 Error connecting to server");
        close(sock);
        exit(EXIT_FAILURE);
    }
    printf("🌐 Connected to server at %s:%d\n", SERVER_IP, SERVER_PORT);

    // Student login
    char roll[50], password[50], name[MAX_LINE], reg_no[MAX_LINE];
    printf("\n🎓 Welcome to ExamSys Online MCQ Exam Platform\n");
    printf("📝 Enter Roll No: ");
    scanf("%s", roll);
    printf("🔒 Enter Password: ");
    clear_input_buffer();
    getPassword(password, sizeof(password));

    // Send login credentials to server
    char login_buf[MAX_LINE];
    snprintf(login_buf, MAX_LINE, "%s|%s", roll, password);
    if (send(sock, login_buf, strlen(login_buf) + 1, 0) < 0) {
        perror("📛 Error sending login data");
        close(sock);
        exit(EXIT_FAILURE);
    }
    printf("📤 Sent login data: %s\n", login_buf);

    // Receive login response from server
    char response[MAX_LINE];
    int n = recv(sock, response, MAX_LINE, 0);
    if (n <= 0) {
        printf("📛 Error receiving login response: %s\n", strerror(errno));
        close(sock);
        exit(EXIT_FAILURE);
    }
    response[n] = '\0';
    printf("📥 Received login response: %s\n", response);

    if (strcmp(response, "INVALID") == 0) {
        printf("📛 Invalid credentials! Exiting.\n");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Parse name and registration number from server response
    sscanf(response, "%[^|]|%s", name, reg_no);
    printf("\n🎉 Login successful. Welcome, %s!\n", name);

    // Wait for instructor to start the exam
    char ready_signal[MAX_LINE];
    printf("\n⏳ Waiting for instructor to start the exam...\n");

    struct timeval timeout;
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    timeout.tv_sec = 300; // Wait up to 5 minutes
    timeout.tv_usec = 0;

    if (select(sock + 1, &readfds, NULL, NULL, &timeout) <= 0) {
        printf("📛 Timeout or error waiting for start signal: %s\n", strerror(errno));
        close(sock);
        exit(EXIT_FAILURE);
    }

    n = recv(sock, ready_signal, MAX_LINE - 1, 0);
    if (n <= 0) {
        printf("📛 Error receiving start signal: %s\n", strerror(errno));
        close(sock);
        exit(EXIT_FAILURE);
    }
    ready_signal[n] = '\0';
    printf("📥 Received signal: %s\n", ready_signal);

    if (strcmp(ready_signal, "START") != 0) {
        printf("📛 Invalid signal received: %s\n", ready_signal);
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Receive exam configuration from server
    int answerTimeout;
    float marksForCorrectAnswer, marksDeductedForWrongAnswer;
    int num_questions;

    n = recv(sock, &answerTimeout, sizeof(int), 0);
    if (n != sizeof(int)) {
        printf("📛 Error receiving answerTimeout: %s (received %d bytes, expected %zu)\n",
               strerror(errno), n, sizeof(int));
        close(sock);
        exit(EXIT_FAILURE);
    }
    if (answerTimeout <= 0 || answerTimeout > 3600) {
        printf("📛 Invalid answerTimeout received: %d, using default: 30\n", answerTimeout);
        answerTimeout = 30;
    }
    printf("📥 Received answerTimeout: %d\n", answerTimeout);

    n = recv(sock, &marksForCorrectAnswer, sizeof(float), 0);
    if (n != sizeof(float)) {
        printf("📛 Error receiving marksForCorrectAnswer: %s (received %d bytes, expected %zu)\n",
               strerror(errno), n, sizeof(float));
        close(sock);
        exit(EXIT_FAILURE);
    }
    if (marksForCorrectAnswer <= 0 || marksForCorrectAnswer > 100) {
        printf("📛 Invalid marksForCorrectAnswer received: %.2f, using default: 1.0\n", marksForCorrectAnswer);
        marksForCorrectAnswer = 1.0;
    }
    printf("📥 Received marksForCorrectAnswer: %.2f\n", marksForCorrectAnswer);

    n = recv(sock, &marksDeductedForWrongAnswer, sizeof(float), 0);
    if (n != sizeof(float)) {
        printf("📛 Error receiving marksDeductedForWrongAnswer: %s (received %d bytes, expected %zu)\n",
               strerror(errno), n, sizeof(float));
        close(sock);
        exit(EXIT_FAILURE);
    }
    if (marksDeductedForWrongAnswer < 0 || marksDeductedForWrongAnswer > 100) {
        printf("📛 Invalid marksDeductedForWrongAnswer received: %.2f, using default: 0.25\n",
               marksDeductedForWrongAnswer);
        marksDeductedForWrongAnswer = 0.25;
    }
    printf("📥 Received marksDeductedForWrongAnswer: %.2f\n", marksDeductedForWrongAnswer);

    n = recv(sock, &num_questions, sizeof(int), 0);
    if (n != sizeof(int)) {
        printf("📛 Error receiving num_questions: %s (received %d bytes, expected %zu)\n",
               strerror(errno), n, sizeof(int));
        close(sock);
        exit(EXIT_FAILURE);
    }
    if (num_questions <= 0 || num_questions > NUM_EXAM_QUESTIONS) {
        printf("📛 Invalid num_questions received: %d, using default: %d\n", num_questions, NUM_EXAM_QUESTIONS);
        num_questions = NUM_EXAM_QUESTIONS;
    }
    printf("📥 Received num_questions: %d\n", num_questions);

    // Allocate memory for questions and receive them from server
    Question *questions = malloc(num_questions * sizeof(Question));
    if (!questions) {
        printf("📛 Error allocating memory for questions\n");
        close(sock);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < num_questions; i++) {
        n = recv(sock, &questions[i], sizeof(Question), 0);
        if (n != sizeof(Question)) {
            printf("📛 Error receiving question %d: %s (received %d bytes, expected %zu)\n",
                   i+1, strerror(errno), n, sizeof(Question));
            free(questions);
            close(sock);
            exit(EXIT_FAILURE);
        }
        // Validate question data
        questions[i].question[MAX_LINE-1] = '\0';
        questions[i].optionA[MAX_LINE-1] = '\0';
        questions[i].optionB[MAX_LINE-1] = '\0';
        questions[i].optionC[MAX_LINE-1] = '\0';
        questions[i].optionD[MAX_LINE-1] = '\0';
        if (questions[i].question[0] == '\0' ||
            !strchr("ABCD", questions[i].correct) ||
            questions[i].difficulty < 1 || questions[i].difficulty > 3) {
            printf("📛 Invalid question %d data, will skip\n", i+1);
            questions[i].question[0] = '\0'; // Mark as invalid
        } else {
            printf("📥 Received question %d: %s\n", i+1, questions[i].question);
        }
    }

    // Print exam rules summary
    printf("\n====================================================\n");
    printf("| 📜          RULES FOR THE EXAM                 |\n");
    printf("====================================================\n");
    printf("| 🔹 Number of questions: %-22d |\n", num_questions);
    printf("| ⏱️  Time per question: %-3d seconds                  |\n", answerTimeout);
    printf("| ➕ Marks for correct answer: %-4.2f                   |\n", marksForCorrectAnswer);
    printf("| ➖ Marks deducted for wrong answer: %-4.2f            |\n", marksDeductedForWrongAnswer);
    printf("====================================================\n");

    // Start overall exam timer in a separate thread
    pthread_t overallTimerThread;
    if (pthread_create(&overallTimerThread, NULL, overall_timer, NULL) != 0) {
        perror("📛 Error creating overall timer thread");
        free(questions);
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Conduct the exam
    conduct_exam(sock, roll, name, questions, num_questions, answerTimeout);

    // Wait for timer thread to finish (if not already)
    pthread_join(overallTimerThread, NULL);
    free(questions);
    close(sock);

    printf("\n✨ Thank you for using ExamSys! Goodbye! ✨\n");
    return 0;
}
