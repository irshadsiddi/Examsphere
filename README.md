# 📝 ExamSphere – Terminal-Based Digital Examination System

**ExamSphere** is a C language-based digital examination platform that runs through the terminal. Designed with a client-server architecture, it ensures secure, efficient, and fair assessments for both students and instructors.

---

## 🚀 Features

- ✅ **Built with C** – Lightweight, fast, and portable.
- 🌐 **Client-Server Architecture** – Robust networking, real-world exam simulation.
- 👨‍🏫 **Instructor Access** – Manage exams, questions, and analytics.
- 🧑‍🎓 **Student Access** – Secure login and seamless exam experience.
- ❌ **Anti-Cheating System** – Flags suspicious activity (e.g., answering in under 5 seconds).
- 📁 **Structured Data Files** – Clean management of students, questions, and results.
- 📊 **Live Dashboard** – Real-time ranking, accuracy, and flag status for instructors.
- 🔒 **Concurrent Connections** – Multi-threaded server supports many students in parallel.
- 📝 **Dynamic Question Bank** – Add, validate, and randomize questions with difficulty levels.
- ⏱️ **Configurable Exam Rules** – Set time limits and marking schemes dynamically.
- 🧾 **Persistent Records** – Results and student data are saved for analytics.
- 🛠️ **Automatic File Recovery** – Creates default files if missing or corrupted.

---

## 🧱 Components

| File / Module                   | Description                                                           |
|----------------------------------|-----------------------------------------------------------------------|
| `server.c`                      | Main server code: authentication, question management, exam control   |
| `client.c`                      | Client code for students (terminal-based exam interface)              |
| `student_dtls.txt`              | Stores student login and identity details                             |
| `instructor_dtls.txt`           | Instructor credentials and permissions                                |
| `questions_with_difficulty.txt`  | Instructor-defined exam questions with difficulty levels              |
| `rules.txt`                     | Exam rules: time limit, marking scheme                                |
| `results.txt`                   | Auto-generated after exam submission                                  |

---

## 🔧 How It Works

1. **Server** initializes, loads/creates data files, and listens for connections.
2. **Instructor** logs in via terminal, manages questions, rules, and exam sessions.
3. **Clients** (students) connect and authenticate using their roll number and password.
4. **Exam**: Server distributes randomized questions, enforces timeouts, and collects answers.
5. **Evaluation**: Auto-grading with negative marking and suspicious activity flagging.
6. **Dashboard**: Instructor can view live rankings, accuracy, and flagged students.

---

## 🗂️ Data File Formats (with Examples)

### `student_dtls.txt`
Stores student login and identity details.  
**Format (one student per line):** 



---
## Images of the project

![alt text](image.png)

![alt text](image-1.png)

