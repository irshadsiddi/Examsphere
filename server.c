#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <time.h>
#include <errno.h>

// Constants for maximum allowed entries and file names
#define MAX_QUESTIONS 200
#define MAX_LINE 512
#define MAX_STUDENTS 100
#define STUDENT_FILE "student_dtls.txt"
#define INSTRUCTOR_FILE "instructor_dtls.txt"
#define QUESTION_FILE "questions_with_difficulty.txt"
#define RESULT_FILE "results.txt"
#define RULES_FILE "rules.txt"
#define NUM_EXAM_QUESTIONS 5
#define SERVER_PORT 8080

// Global variables for exam configuration and state
int answerTimeout = 30; // Time allowed per question in seconds
float marksForCorrectAnswer = 1.0; // Marks for correct answer
float marksDeductedForWrongAnswer = 0.25; // Negative marks for wrong answer
volatile int examStarted = 0; // Flag to indicate if exam has started (shared between threads)
pthread_mutex_t exam_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for exam state
pthread_cond_t exam_cond = PTHREAD_COND_INITIALIZER;    // Condition variable for exam start

// Data structures

// Holds student details as read from file
typedef struct {
    char name[MAX_LINE];
    char roll[MAX_LINE];
    char reg_no[MAX_LINE];
    char password[MAX_LINE];
} Student;

// Holds instructor details as read from file
typedef struct {
    char name[MAX_LINE];
    char instructor_id[MAX_LINE];
    char password[MAX_LINE];
} Instructor;

// Holds a single exam question, options, correct answer, and difficulty
typedef struct {
    char question[MAX_LINE];
    char optionA[MAX_LINE];
    char optionB[MAX_LINE];
    char optionC[MAX_LINE];
    char optionD[MAX_LINE];
    char correct;           // 'A', 'B', 'C', or 'D'
    char padding1[3];       // Padding for alignment
    int difficulty;         // 1 (easy), 2 (medium), 3 (hard)
} Question;

// Holds a student's exam results for dashboard and ranking
typedef struct {
    char roll[MAX_LINE];
    char name[MAX_LINE];
    int responseTimes[NUM_EXAM_QUESTIONS]; // Time taken per question
    int totalTime;                         // Total time for exam
    int correctAnswers;                    // Number of correct answers
    int totalQuestions;                    // Number of questions attempted
    int rank;                              // Rank after sorting
    int flagged;                           // 1 if suspicious, 0 otherwise
} DashboardStudent;

// Holds info about a connected client (student)
typedef struct {
    int sock;                  // Socket descriptor
    char roll[MAX_LINE];       // Student roll number
} Client;

// Arrays and counters for students, questions, and clients
DashboardStudent dashboardStudents[MAX_STUDENTS]; // All students' dashboard data
int studentCount = 0;                             // Number of students in dashboard
Question questions[MAX_QUESTIONS];                // All loaded questions
int totalQuestions = 0;                           // Number of loaded questions
Client clients[MAX_STUDENTS];                     // Connected clients
int clientCount = 0;                              // Number of connected clients
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for client list

// Utility: Clears stdin buffer to avoid leftover input from previous scanf/fgets
void clear_input_buffer() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

// Utility: Reads a password from the user without echoing input on terminal
void getPassword(char *password, int size) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt); // Save current terminal settings
    newt = oldt;
    newt.c_lflag &= ~(ECHO); // Disable echo
    tcsetattr(STDIN_FILENO, TCSANOW, &newt); // Apply new settings

    if (fgets(password, size, stdin) != NULL) {
        password[strcspn(password, "\n")] = '\0'; // Remove newline
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restore old settings
    printf("\n");
}

// Utility: Prints a hexadecimal dump of a memory buffer (for debugging)
void log_hexdump(const void *data, size_t size) {
    const unsigned char *buf = (const unsigned char *)data;
    for (size_t i = 0; i < size; i++) {
        printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
}

// Loads exam rules (time limit, marking scheme) from file or creates default if missing
void load_rules() {
    FILE *fp = fopen(RULES_FILE, "r");
    answerTimeout = 30;
    marksForCorrectAnswer = 1.0;
    marksDeductedForWrongAnswer = 0.25;

    if (fp == NULL) {
        // File missing: create with defaults
        printf("📛 Rules file not found, creating with defaults\n");
        fp = fopen(RULES_FILE, "w");
        if (fp == NULL) {
            perror("📛 Error creating rules file");
            return;
        }
        fprintf(fp, "Time limit per question: %d\nMarks awarded for correct answer: %.2f\nMarks deducted for incorrect answer: %.2f\n", 
                answerTimeout, marksForCorrectAnswer, marksDeductedForWrongAnswer);
        fclose(fp);
    } else {
        // File exists: read and validate each rule line
        char buffer[MAX_LINE];
        printf("📜 Reading rules file:\n");
        if (fgets(buffer, MAX_LINE, fp)) {
            printf("  %s", buffer);
            buffer[strcspn(buffer, "\n")] = '\0';
            if (sscanf(buffer, "Time limit per question: %d", &answerTimeout) != 1 || 
                answerTimeout <= 0 || answerTimeout > 3600) {
                printf("📛 Invalid answerTimeout in file, using default: 30\n");
                answerTimeout = 30;
            }
        }
        if (fgets(buffer, MAX_LINE, fp)) {
            printf("  %s", buffer);
            buffer[strcspn(buffer, "\n")] = '\0';
            if (sscanf(buffer, "Marks awarded for correct answer: %f", &marksForCorrectAnswer) != 1 || 
                marksForCorrectAnswer <= 0 || marksForCorrectAnswer > 100) {
                printf("📛 Invalid marksForCorrectAnswer in file, using default: 1.0\n");
                marksForCorrectAnswer = 1.0;
            }
        }
        if (fgets(buffer, MAX_LINE, fp)) {
            printf("  %s", buffer);
            buffer[strcspn(buffer, "\n")] = '\0';
            if (sscanf(buffer, "Marks deducted for incorrect answer: %f", &marksDeductedForWrongAnswer) != 1 || 
                marksDeductedForWrongAnswer < 0 || marksDeductedForWrongAnswer > 100) {
                printf("📛 Invalid marksDeductedForWrongAnswer in file, using default: 0.25\n");
                marksDeductedForWrongAnswer = 0.25;
            }
        }
        fclose(fp);
    }
    printf("📜 Loaded rules: Timeout=%d, Correct=%.2f, Wrong=%.2f\n", answerTimeout, marksForCorrectAnswer, marksDeductedForWrongAnswer);
}

// Utility: Removes leading/trailing whitespace and newline from a string
void trim(char *s) {
    s[strcspn(s, "\n")] = '\0';
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
}

// Reads a non-empty, non-blank line from a file into buffer, skipping empty lines
int read_nonempty_line(FILE *fp, char *buffer, int size) {
    while (fgets(buffer, size, fp)) {
        buffer[strcspn(buffer, "\n")] = '\0';
        char temp[MAX_LINE];
        strcpy(temp, buffer);
        char *start = temp;
        while (*start && isspace((unsigned char)*start)) start++;
        if (strlen(start) > 0) {
            strcpy(buffer, start);
            return 1;
        }
    }
    return 0;
}

// Loads questions from the question file into the questions array.
// If file is missing or incomplete, creates default questions.
void load_questions() {
    FILE *fp = fopen(QUESTION_FILE, "r");
    if (fp == NULL) {
        // File missing: create with default question
        printf("📛 Questions file not found, creating with default question\n");
        fp = fopen(QUESTION_FILE, "w");
        if (fp == NULL) {
            perror("📛 Error creating questions file");
            exit(EXIT_FAILURE);
        }
        fprintf(fp, "What is the default question?\nOption A\nOption B\nOption C\nOption D\nA\n1\n");
        fclose(fp);
        fp = fopen(QUESTION_FILE, "r");
        if (fp == NULL) {
            perror("📛 Error reopening questions file");
            exit(EXIT_FAILURE);
        }
    }

    char line[MAX_LINE];
    int qIndex = 0;
    int line_number = 0;
    printf("📜 Reading questions file:\n");
    while (qIndex < MAX_QUESTIONS) {
        Question q = {0}; // Initialize to zero
        line_number++;
        if (!read_nonempty_line(fp, line, MAX_LINE)) {
            printf("📛 EOF or error at line %d\n", line_number);
            break;
        }
        printf("  Line %d: %s\n", line_number, line);
        strncpy(q.question, line, MAX_LINE - 1);
        q.question[MAX_LINE - 1] = '\0';

        line_number++;
        if (!read_nonempty_line(fp, line, MAX_LINE)) {
            printf("📛 Missing optionA at line %d\n", line_number);
            break;
        }
        printf("  Line %d: %s\n", line_number, line);
        strncpy(q.optionA, line, MAX_LINE - 1);
        q.optionA[MAX_LINE - 1] = '\0';

        line_number++;
        if (!read_nonempty_line(fp, line, MAX_LINE)) {
            printf("📛 Missing optionB at line %d\n", line_number);
            break;
        }
        printf("  Line %d: %s\n", line_number, line);
        strncpy(q.optionB, line, MAX_LINE - 1);
        q.optionB[MAX_LINE - 1] = '\0';

        line_number++;
        if (!read_nonempty_line(fp, line, MAX_LINE)) {
            printf("📛 Missing optionC at line %d\n", line_number);
            break;
        }
        printf("  Line %d: %s\n", line_number, line);
        strncpy(q.optionC, line, MAX_LINE - 1);
        q.optionC[MAX_LINE - 1] = '\0';

        line_number++;
        if (!read_nonempty_line(fp, line, MAX_LINE)) {
            printf("📛 Missing optionD at line %d\n", line_number);
            break;
        }
        printf("  Line %d: %s\n", line_number, line);
        strncpy(q.optionD, line, MAX_LINE - 1);
        q.optionD[MAX_LINE - 1] = '\0';

        line_number++;
        if (!read_nonempty_line(fp, line, MAX_LINE)) {
            printf("📛 Missing correct answer at line %d\n", line_number);
            break;
        }
        printf("  Line %d: %s\n", line_number, line);
        q.correct = toupper(line[0]);

        line_number++;
        if (!read_nonempty_line(fp, line, MAX_LINE)) {
            printf("📛 Missing difficulty at line %d\n", line_number);
            break;
        }
        printf("  Line %d: %s\n", line_number, line);
        q.difficulty = atoi(line);

        // Validate question structure and content
        if (q.question[0] == '\0' || q.optionA[0] == '\0' || q.optionB[0] == '\0' ||
            q.optionC[0] == '\0' || q.optionD[0] == '\0' || !strchr("ABCD", q.correct) || q.difficulty < 1 || q.difficulty > 3) {
            printf("📛 Skipping invalid question %d at line %d: %s\n", 
                   qIndex + 1, line_number - 6, q.question[0] ? q.question : "<empty>");
            continue;
        }

        questions[qIndex] = q;
        printf("📚 Loaded question %d: %s (Correct: %c, Difficulty: %d)\n", 
               qIndex + 1, q.question, q.correct, q.difficulty);
        qIndex++;
    }

    totalQuestions = qIndex;
    fclose(fp);
    printf("📚 Total loaded questions: %d\n", totalQuestions);
    // If not enough questions, add default ones
    if (totalQuestions < NUM_EXAM_QUESTIONS) {
        printf("📛 Warning: Not enough questions (%d < %d), adding default\n", totalQuestions, NUM_EXAM_QUESTIONS);
        while (totalQuestions < NUM_EXAM_QUESTIONS && totalQuestions < MAX_QUESTIONS) {
            Question default_q = {
                .question = "What is the default question?",
                .optionA = "Option A",
                .optionB = "Option B",
                .optionC = "Option C",
                .optionD = "Option D",
                .correct = 'A',
                .difficulty = 1
            };
            questions[totalQuestions] = default_q;
            printf("📚 Added default question %d: %s\n", totalQuestions + 1, default_q.question);
            totalQuestions++;
        }
    }
}

// Verifies student credentials by matching roll and password from the student details file.
// On success, copies the student's name and registration number to output parameters.
int verify_student(const char *roll, const char *pass, char *name, char *reg_no) {
    FILE *fp = fopen(STUDENT_FILE, "r");
    if (fp == NULL) {
        perror("📛 Error opening student details file");
        return 0;
    }
    char fileName[50], fileRoll[50], fileRegNo[50], filePass[50];
    int valid = 0;
    while (fscanf(fp, "%49s %49s %49s %49s", fileName, fileRoll, fileRegNo, filePass) == 4) {
        if (strcmp(fileRoll, roll) == 0 && strcmp(filePass, pass) == 0) {
            strncpy(name, fileName, 49);
            name[49] = '\0';
            strncpy(reg_no, fileRegNo, 49);
            reg_no[49] = '\0';
            valid = 1;
            break;
        }
    }
    fclose(fp);
    return valid;
}

// Verifies instructor credentials by matching ID and password from the instructor details file.
// On success, copies the instructor's name to the output parameter.
int verify_instructor(const char *instructor_id, const char *pass, char *name) {
    FILE *fp = fopen(INSTRUCTOR_FILE, "r");
    if (fp == NULL) {
        perror("📛 Error opening instructor details file");
        return 0;
    }
    char fileName[50], fileInstructorID[50], filePass[50];
    int valid = 0;
    while (fscanf(fp, "%49s %49s %49s", fileName, fileInstructorID, filePass) == 3) {
        if (strcmp(fileInstructorID, instructor_id) == 0 && strcmp(filePass, pass) == 0) {
            strncpy(name, fileName, 49);
            name[49] = '\0';
            valid = 1;
            break;
        }
    }
    fclose(fp);
    return valid;
}

// Appends a student's exam result to the results file, using file locking for concurrency safety.
void append_result(DashboardStudent *s) {
    FILE *fp = fopen(RESULT_FILE, "a");
    if (fp == NULL) {
        perror("📛 Error opening result file");
        return;
    }
    int fd = fileno(fp);
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    fcntl(fd, F_SETLKW, &lock);

    fprintf(fp, "%s|%s|%d|%d|%d|%d|", 
           s->roll, s->name, s->correctAnswers, s->totalQuestions, s->flagged, s->totalTime);

    for (int i = 0; i < s->totalQuestions; i++) {
        fprintf(fp, "%d,", s->responseTimes[i]);
    }
    fprintf(fp, "\n");

    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);
    fclose(fp);
}

// Loads all dashboard data (student results) from the results file into memory.
void loadDashboardData() {
    FILE *file = fopen(RESULT_FILE, "r");
    if (file == NULL) return;

    studentCount = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), file) && studentCount < MAX_STUDENTS) {
        DashboardStudent *s = &dashboardStudents[studentCount];
        char *token = strtok(line, "|");

        strncpy(s->roll, token, MAX_LINE - 1);
        s->roll[MAX_LINE - 1] = '\0';
        token = strtok(NULL, "|");
        strncpy(s->name, token, MAX_LINE - 1);
        s->name[MAX_LINE - 1] = '\0';
        token = strtok(NULL, "|");
        s->correctAnswers = atoi(token);
        token = strtok(NULL, "|");
        s->totalQuestions = atoi(token);
        token = strtok(NULL, "|");
        s->flagged = atoi(token);
        token = strtok(NULL, "|");
        s->totalTime = atoi(token);

        char *timeToken = strtok(NULL, ",");
        int i = 0;
        while (timeToken != NULL && i < NUM_EXAM_QUESTIONS) {
            s->responseTimes[i++] = atoi(timeToken);
            timeToken = strtok(NULL, ",");
        }

        studentCount++;
    }
    fclose(file);
}

// Flags students as suspicious if any of their response times are below 2 seconds.
void flagSuspiciousActivity() {
    for (int i = 0; i < studentCount; ++i) {
        for (int j = 0; j < dashboardStudents[i].totalQuestions; ++j) {
            if (dashboardStudents[i].responseTimes[j] < 2) {
                dashboardStudents[i].flagged = 1;
                break;
            }
        }
    }
}

// Ranks students in the dashboard by number of correct answers (descending).
void rankStudents() {
    for (int i = 0; i < studentCount - 1; ++i) {
        for (int j = 0; j < studentCount - i - 1; ++j) {
            if (dashboardStudents[j].correctAnswers < dashboardStudents[j + 1].correctAnswers) {
                DashboardStudent temp = dashboardStudents[j];
                dashboardStudents[j] = dashboardStudents[j + 1];
                dashboardStudents[j + 1] = temp;
            }
        }
    }

    for (int i = 0; i < studentCount; ++i) {
        dashboardStudents[i].rank = i + 1;
    }
}

// Displays the dashboard with student ranks, times, accuracy, and flagged status.
void displayDashboard() {
    loadDashboardData();
    flagSuspiciousActivity();
    rankStudents();

    printf("\n\n--------------------------------------------------\n");
    printf("| Rank | Name         | Total Time | Accuracy | Flagged |\n");
    printf("--------------------------------------------------\n");

    for (int i = 0; i < studentCount; ++i) {
        float accuracy = dashboardStudents[i].totalQuestions > 0 ? 
            (float)dashboardStudents[i].correctAnswers / dashboardStudents[i].totalQuestions * 100 : 0;
        printf("| %-4d | %-12s | %-10d | %-8.2f | %-7s |\n",
               dashboardStudents[i].rank,
               dashboardStudents[i].name,
               dashboardStudents[i].totalTime,
               accuracy,
               dashboardStudents[i].flagged ? "🚩" : "✅");
    }
    printf("--------------------------------------------------\n");
}

// Prompts instructor to add a new question and appends it to the question file after validation.
void add_question() {
    FILE *fp = fopen(QUESTION_FILE, "a");
    if (fp == NULL) {
        perror("📛 Error opening questions file");
        return;
    }

    Question newQuestion = {0};
    printf("📝 Enter the question: ");
    clear_input_buffer();
    fgets(newQuestion.question, MAX_LINE, stdin);
    trim(newQuestion.question);

    printf("🅰️  Enter option A: ");
    fgets(newQuestion.optionA, MAX_LINE, stdin);
    trim(newQuestion.optionA);

    printf("🅱️  Enter option B: ");
    fgets(newQuestion.optionB, MAX_LINE, stdin);
    trim(newQuestion.optionB);

    printf("©️  Enter option C: ");
    fgets(newQuestion.optionC, MAX_LINE, stdin);
    trim(newQuestion.optionC);

    printf("🅳  Enter option D: ");
    fgets(newQuestion.optionD, MAX_LINE, stdin);
    trim(newQuestion.optionD);

    printf("✅ Enter the correct option (A/B/C/D): ");
    scanf(" %c", &newQuestion.correct);
    newQuestion.correct = toupper(newQuestion.correct);

    printf("📊 Enter difficulty level (1=Easy, 2=Medium, 3=Hard): ");
    scanf("%d", &newQuestion.difficulty);

    if (newQuestion.question[0] == '\0' || newQuestion.optionA[0] == '\0' ||
        newQuestion.optionB[0] == '\0' || newQuestion.optionC[0] == '\0' ||
        newQuestion.optionD[0] == '\0' || !strchr("ABCD", newQuestion.correct) ||
        newQuestion.difficulty < 1 || newQuestion.difficulty > 3) {
        printf("📛 Invalid question data, not added\n");
        fclose(fp);
        return;
    }

    fprintf(fp, "%s\n%s\n%s\n%s\n%s\n%c\n%d\n", 
           newQuestion.question, 
           newQuestion.optionA,
           newQuestion.optionB,
           newQuestion.optionC,
           newQuestion.optionD,
           newQuestion.correct,
           newQuestion.difficulty);

    fclose(fp);
    printf("🎉 Question added successfully!\n");
}

// Sets the time limit per question and updates the rules file.
void set_time_limit() {
    int new_time;
    printf("⏱️  Enter the new time limit for each question (in seconds): ");
    scanf("%d", &new_time);
    if (new_time <= 0 || new_time > 3600) {
        printf("📛 Invalid time limit, using default: 30 seconds\n");
        new_time = 30;
    }
    answerTimeout = new_time;

    FILE *fp = fopen(RULES_FILE, "w");
    if (fp == NULL) {
        perror("📛 Error writing rules file");
        return;
    }
    fprintf(fp, "Time limit per question: %d\nMarks awarded for correct answer: %.2f\nMarks deducted for incorrect answer: %.2f\n", 
            answerTimeout, marksForCorrectAnswer, marksDeductedForWrongAnswer);
    fclose(fp);
    printf("🔄 Time limit set to %d seconds.\n", answerTimeout);
}

// Sets the marking scheme for correct and wrong answers, and updates the rules file.
void set_marking_scheme() {
    printf("➕ Enter marks for correct answer: ");
    scanf("%f", &marksForCorrectAnswer);
    if (marksForCorrectAnswer <= 0 || marksForCorrectAnswer > 100) {
        printf("📛 Invalid marks, using default: 1.0\n");
        marksForCorrectAnswer = 1.0;
    }
    printf("➖ Enter marks deducted for wrong answer: ");
    scanf("%f", &marksDeductedForWrongAnswer);
    if (marksDeductedForWrongAnswer < 0 || marksDeductedForWrongAnswer > 100) {
        printf("📛 Invalid marks, using default: 0.25\n");
        marksDeductedForWrongAnswer = 0.25;
    }

    FILE *fp = fopen(RULES_FILE, "w");
    if (fp == NULL) {
        perror("📛 Error writing rules file");
        return;
    }
    fprintf(fp, "Time limit per question: %d\nMarks awarded for correct answer: %.2f\nMarks deducted for incorrect answer: %.2f\n", 
            answerTimeout, marksForCorrectAnswer, marksDeductedForWrongAnswer);
    fclose(fp);
    printf("🔄 Marking scheme updated: +%.2f for correct, -%.2f for wrong.\n", 
           marksForCorrectAnswer, marksDeductedForWrongAnswer);
}

// Sends exam configuration and selected questions to a connected student client.
void send_exam_data(int client_sock) {
    printf("📤 Sending exam data to socket %d\n", client_sock);
    int valid_answerTimeout = 30;
    float valid_marksForCorrectAnswer = 1.0;
    float valid_marksDeductedForWrongAnswer = 0.25;
    int num_questions = NUM_EXAM_QUESTIONS;

    // Validate rules before sending
    if (answerTimeout > 0 && answerTimeout <= 3600) valid_answerTimeout = answerTimeout;
    if (marksForCorrectAnswer > 0 && marksForCorrectAnswer <= 100) valid_marksForCorrectAnswer = marksForCorrectAnswer;
    if (marksDeductedForWrongAnswer >= 0 && marksDeductedForWrongAnswer <= 100) valid_marksDeductedForWrongAnswer = marksDeductedForWrongAnswer;
    if (totalQuestions < NUM_EXAM_QUESTIONS) {
        printf("📛 Warning: Only %d questions available\n", totalQuestions);
        num_questions = totalQuestions;
    }

    printf("📜 Rules: Timeout=%d, Correct=%.2f, Wrong=%.2f, Questions=%d\n",
           valid_answerTimeout, valid_marksForCorrectAnswer, valid_marksDeductedForWrongAnswer, num_questions);

    if (send(client_sock, &valid_answerTimeout, sizeof(int), 0) != sizeof(int)) {
        perror("📛 Error sending answerTimeout");
        return;
    }
    printf("📤 Sent answerTimeout: %d\n", valid_answerTimeout);

    if (send(client_sock, &valid_marksForCorrectAnswer, sizeof(float), 0) != sizeof(float)) {
        perror("📛 Error sending marksForCorrectAnswer");
        return;
    }
    printf("📤 Sent marksForCorrectAnswer: %.2f\n", valid_marksForCorrectAnswer);

    if (send(client_sock, &valid_marksDeductedForWrongAnswer, sizeof(float), 0) != sizeof(float)) {
        perror("📛 Error sending marksDeductedForWrongAnswer");
        return;
    }
    printf("📤 Sent marksDeductedForWrongAnswer: %.2f\n", valid_marksDeductedForWrongAnswer);

    if (send(client_sock, &num_questions, sizeof(int), 0) != sizeof(int)) {
        perror("📛 Error sending num_questions");
        return;
    }
    printf("📤 Sent num_questions: %d\n", num_questions);

    // Shuffle and select questions to send
    int indices[MAX_QUESTIONS];
    for (int i = 0; i < totalQuestions; i++) {
        indices[i] = i;
    }
    srand(time(NULL));
    for (int i = totalQuestions - 1; i > 0 && i >= totalQuestions - num_questions; i--) {
        int j = rand() % (i + 1);
        int temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }

    for (int i = 0; i < num_questions; i++) {
        Question *q = &questions[indices[i]];
        if (q->question[0] == '\0' || !strchr("ABCD", q->correct) || q->difficulty < 1 || q->difficulty > 3) {
            printf("📛 Invalid question %d, sending default\n", i+1);
            Question default_q = {
                .question = "What is the default question?",
                .optionA = "Option A",
                .optionB = "Option B",
                .optionC = "Option C",
                .optionD = "Option D",
                .correct = 'A',
                .difficulty = 1
            };
            q = &default_q;
        }
        printf("📤 Sending question %d: %s\n", i+1, q->question);
        printf("📤 Question %d hexdump:\n", i+1);
        log_hexdump(q, sizeof(Question));
        ssize_t sent = send(client_sock, q, sizeof(Question), 0);
        if (sent != sizeof(Question)) {
            fprintf(stderr, "📛 Error sending question %d: sent %zd bytes, expected %zu\n", i+1, sent, sizeof(Question));
            return;
        }
        printf("📤 Sent question %d: %s (%zd bytes)\n", i+1, q->question, sent);
    }
}

// Starts the exam for all registered students, sending them the START signal and exam data.
void start_exam() {
    pthread_mutex_lock(&clients_mutex);
    if (clientCount == 0) {
        printf("📛 No students registered for the exam.\n");
        pthread_mutex_unlock(&clients_mutex);
        return;
    }
    printf("📢 Starting exam for %d registered students...\n", clientCount);
    pthread_mutex_lock(&exam_mutex);
    examStarted = 1;
    pthread_cond_broadcast(&exam_cond);
    pthread_mutex_unlock(&exam_mutex);

    for (int i = 0; i < clientCount; i++) {
        printf("📢 Sending START to client %s (socket %d)\n", clients[i].roll, clients[i].sock);
        if (send(clients[i].sock, "START", 6, 0) != 6) {
            perror("📛 Error sending START signal");
        } else {
            printf("✅ START sent to client %s\n", clients[i].roll);
            send_exam_data(clients[i].sock);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Handles an individual client (student) connection in a separate thread.
// Manages login, registration, exam waiting, receiving results, and cleanup.
void *handle_client(void *arg) {
    int client_sock = *(int*)arg;
    free(arg);

    printf("📥 Handling new client (socket %d)\n", client_sock);

    char buffer[MAX_LINE];
    int n = recv(client_sock, buffer, MAX_LINE - 1, 0);
    if (n <= 0) {
        printf("📛 Error receiving login data: %s\n", strerror(errno));
        close(client_sock);
        return NULL;
    }
    buffer[n] = '\0';
    printf("📥 Received login data: %s\n", buffer);

    char roll[50], password[50], name[50], reg_no[50];
    sscanf(buffer, "%49[^|]|%49s", roll, password);

    if (!verify_student(roll, password, name, reg_no)) {
        printf("📛 Invalid credentials for roll %s\n", roll);
        if (send(client_sock, "INVALID", 8, 0) != 8) {
            perror("📛 Error sending INVALID response");
        }
        close(client_sock);
        return NULL;
    }

    char response[100]; // Smaller buffer since name and reg_no are max 49 each
    snprintf(response, sizeof(response), "%s|%s", name, reg_no);
    if (send(client_sock, response, strlen(response) + 1, 0) != strlen(response) + 1) {
        perror("📛 Error sending login response");
        close(client_sock);
        return NULL;
    }
    printf("📤 Sent login response: %s\n", response);

    pthread_mutex_lock(&clients_mutex);
    clients[clientCount].sock = client_sock;
    strncpy(clients[clientCount].roll, roll, MAX_LINE - 1);
    clients[clientCount].roll[MAX_LINE - 1] = '\0';
    clientCount++;
    printf("🎉 Student %s (Roll: %s) registered. Total clients: %d\n", name, roll, clientCount);
    pthread_mutex_unlock(&clients_mutex);

    pthread_mutex_lock(&exam_mutex);
    while (!examStarted) {
        printf("⏳ Client %s (socket %d) waiting for exam start\n", roll, client_sock);
        pthread_cond_wait(&exam_cond, &exam_mutex);
    }
    pthread_mutex_unlock(&exam_mutex);

    DashboardStudent result;
    n = recv(client_sock, &result, sizeof(DashboardStudent), 0);
    if (n <= 0) {
        printf("📛 Error receiving exam result for roll %s: %s\n", roll, strerror(errno));
    } else {
        printf("📥 Received exam result for roll %s\n", roll);
        append_result(&result);
    }

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < clientCount; i++) {
        if (clients[i].sock == client_sock) {
            printf("🗑️ Removing client %s (socket %d)\n", clients[i].roll, client_sock);
            for (int j = i; j < clientCount - 1; j++) {
                clients[j] = clients[j + 1];
            }
            clientCount--;
            break;
        }
    }
    printf("📊 Total clients after removal: %d\n", clientCount);
    pthread_mutex_unlock(&clients_mutex);

    close(client_sock);
    printf("🔌 Closed client socket %d\n", client_sock);
    return NULL;
}

// Provides the instructor with a menu to manage the exam system (set time, add questions, marking, dashboard, start exam).
void instructor_menu() {
    int instructor_choice;
    do {
        printf("\n📋 Instructor Menu:\n");
        printf("1. ⏱️  Set Time Limit for Questions\n");
        printf("2. 📝 Add a Question\n");
        printf("3. 📊 Set Marking Scheme\n");
        printf("4. 📈 View Dashboard\n");
        printf("5. 📢 Start Exam\n");
        printf("6. 🚪 Exit\n");
        printf("🎯 Enter your choice: ");
        scanf("%d", &instructor_choice);

        switch (instructor_choice) {
            case 1:
                set_time_limit();
                break;
            case 2:
                add_question();
                load_questions();
                break;
            case 3:
                set_marking_scheme();
                break;
            case 4:
                displayDashboard();
                break;
            case 5:
                start_exam();
                break;
            case 6:
                printf("\n🚪 Exiting...\n");
                break;
            default:
                printf("\n📛 Invalid choice! Please try again.\n");
        }
        clear_input_buffer();
    } while (instructor_choice != 6);
}

// Main function: initializes server, handles instructor login, starts instructor menu and client threads.
int main() {
    printf("\n\n✨✨✨ Welcome to ExamSys - Instructor Server ✨✨✨\n\n");
    printf("📏 Size of Question: %zu bytes\n", sizeof(Question));
    printf("📏 Size of DashboardStudent: %zu bytes\n", sizeof(DashboardStudent));

    load_rules();
    load_questions();

    char instructor_id[50], password[50], name[50];
    printf("\n👨‍🏫 Enter Instructor ID: ");
    scanf("%s", instructor_id);
    printf("🔒 Enter Password: ");
    clear_input_buffer();
    getPassword(password, sizeof(password));

    if (!verify_instructor(instructor_id, password, name)) {
        printf("\n📛 Invalid credentials! Exiting.\n");
        exit(EXIT_FAILURE);
    }

    printf("\n🎉 Login successful. Welcome, %s!\n", name);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("📛 Error creating socket");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("📛 Error setting socket options");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("📛 Error binding socket");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, 10) < 0) {
        perror("📛 Error listening on socket");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    printf("🌐 Server listening on port %d...\n", SERVER_PORT);

    pthread_t instructor_thread;
    if (pthread_create(&instructor_thread, NULL, (void*(*)(void*))instructor_menu, NULL) != 0) {
        perror("📛 Error creating instructor thread");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // Main server loop: accept and handle student clients
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int *client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (*client_sock < 0) {
            perror("📛 Error accepting client");
            free(client_sock);
            continue;
        }
        printf("📥 Accepted new client connection (socket %d)\n", *client_sock);

        pthread_t client_thread;
        if (pthread_create(&client_thread, NULL, handle_client, client_sock) != 0) {
            perror("📛 Error creating client thread");
            close(*client_sock);
            free(client_sock);
        } else {
            pthread_detach(client_thread);
        }
    }

    close(server_sock);
    pthread_join(instructor_thread, NULL);
    return 0;
}
