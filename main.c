#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sensors/sensors.h>
#include <sensors/error.h>
#include <unistd.h>
#include <ctype.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>

#define VERSION "1.0.0"
#define MAX_CPUS 256 // Maximum number of CPU cores supported
#define BAR_WIDTH 40 // Width of the usage bar
#define COLOR_RESET "\033[0m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED "\033[31m"
#define TERM_WIDTH_FALLBACK 80
#define TIME_BETWEEN_SAMPLES_US 200000 // 200ms
#define TIME_BETWEEN_KEY_POLL_US 50000 // 50ms
#define KEY_ESC 27
#define STAT_FILE "/proc/stat"

// Helper function: Print a colored progress bar for CPU usage
// Prints a horizontal bar with color depending on the usage percentage.
void print_bar(float percent)
{
    // Calculate how many bar segments should be filled
    int filled = (int)(percent * BAR_WIDTH / 100.0f);
    const char *color;
    // Choose color based on usage percentage
    if (percent < 50)
        color = COLOR_GREEN;
    else if (percent < 80)
        color = COLOR_YELLOW;
    else
        color = COLOR_RED;
    printf("%s[", color);
    for (int i = 0; i < BAR_WIDTH; ++i)
    {
        if (i < filled)
            printf("█");
        else
            printf(" ");
    }
    printf("]%s", COLOR_RESET);
}

// Helper function: Print a formatted line centered in the terminal
// Uses variable arguments to format the string, then centers it based on terminal width.
int print_centered(const char *fmt, ...)
{
    struct winsize w;
    // Try to get terminal width
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1)
    {
        fprintf(stderr, "Error: Could not get terminal size: %s\n", strerror(errno));
        return -1;
    }
    int width = w.ws_col > 0 ? w.ws_col : TERM_WIDTH_FALLBACK;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    // Format the string
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0)
    {
        fprintf(stderr, "Error: Formatting failed.\n");
        return -1;
    }
    int len = strlen(buf);
    int pad = (width - len) / 2;
    // Print padding and then the string
    if (pad > 0)
        printf("%*s%s", pad, "", buf);
    else
        printf("%s", buf);
    return 0;
}

// Reads CPU usage statistics per core from /proc/stat
// If cpu_ids is NULL or *num_cpus == 0, it initializes cpu_ids and returns the number of CPUs
// Otherwise, it updates the usage values for the given cpu_ids
int read_cpu_stats(unsigned long long user[], unsigned long long nice[], unsigned long long system[], unsigned long long idle[], int cpu_ids[], int *num_cpus)
{
    // Open /proc/stat for reading
    FILE *fp = fopen(STAT_FILE, "r");
    if (!fp)
    {
        fprintf(stderr, "Error: Could not open %s: %s\n", STAT_FILE, strerror(errno));
        return -1;
    }
    char line[256];
    if (cpu_ids == NULL || *num_cpus == 0)
    {
        int found_cpus = 0;
        // Read each line and parse CPU stats
        while (fgets(line, sizeof(line), fp))
        {
            // Only consider lines starting with "cpu" followed by a digit
            if (strncmp(line, "cpu", 3) != 0 || !isdigit(line[3]))
                continue;
            int cpu_id;
            unsigned long long u, n, s, idl;
            // Parse CPU stats
            if (sscanf(line, "cpu%d %llu %llu %llu %llu", &cpu_id, &u, &n, &s, &idl) == 5 && found_cpus < MAX_CPUS)
            {
                cpu_ids[found_cpus] = cpu_id;
                user[found_cpus] = u;
                nice[found_cpus] = n;
                system[found_cpus] = s;
                idle[found_cpus] = idl;
                found_cpus++;
            }
        }
        *num_cpus = found_cpus;
    }
    else
    {
        int found = 0;
        // Update stats for known CPUs
        while (fgets(line, sizeof(line), fp))
        {
            if (strncmp(line, "cpu", 3) != 0 || !isdigit(line[3]))
                continue;
            int cpu_id;
            unsigned long long u, n, s, idl;
            if (sscanf(line, "cpu%d %llu %llu %llu %llu", &cpu_id, &u, &n, &s, &idl) == 5)
            {
                for (int i = 0; i < *num_cpus; ++i)
                {
                    if (cpu_id == cpu_ids[i])
                    {
                        user[i] = u;
                        nice[i] = n;
                        system[i] = s;
                        idle[i] = idl;
                        found++;
                        break;
                    }
                }
                if (found == *num_cpus)
                    break;
            }
        }
    }
    // Close the file
    if (fclose(fp) != 0)
    {
        fprintf(stderr, "Error: Could not close %s: %s\n", STAT_FILE, strerror(errno));
    }
    return 0;
}

// For each core, print frequency, usage and progress bar in one centered line
// Reads CPU stats twice to calculate usage, then prints a line for each core with usage and frequency
void print_core_usage_bars()
{
    unsigned long long user1[MAX_CPUS], nice1[MAX_CPUS], system1[MAX_CPUS], idle1[MAX_CPUS];
    unsigned long long user2[MAX_CPUS], nice2[MAX_CPUS], system2[MAX_CPUS], idle2[MAX_CPUS];
    int cpu_ids[MAX_CPUS];
    int num_cpus = 0;
    // First read: get initial CPU stats and core IDs
    if (read_cpu_stats(user1, nice1, system1, idle1, cpu_ids, &num_cpus) != 0)
    {
        fprintf(stderr, "Error: Could not read CPU statistics.\n");
        return;
    }
    // Wait for the next sample
    if (usleep(TIME_BETWEEN_SAMPLES_US) != 0)
    {
        fprintf(stderr, "Error: usleep failed: %s\n", strerror(errno));
    }
    // Second read: get updated CPU stats
    if (read_cpu_stats(user2, nice2, system2, idle2, cpu_ids, &num_cpus) != 0)
    {
        fprintf(stderr, "Error: Could not read CPU statistics (second measurement).\n");
        return;
    }
    printf("\n");
    struct winsize w;
    // Get terminal width for centering
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1)
    {
        fprintf(stderr, "Error: Could not get terminal size: %s\n", strerror(errno));
        w.ws_col = TERM_WIDTH_FALLBACK;
    }
    int width = w.ws_col > 0 ? w.ws_col : TERM_WIDTH_FALLBACK;
    char line_template[256];
    int n = snprintf(line_template, sizeof(line_template), "CPU %-3d %6.1f%%  %8.2f MHz  ", 0, 0.0, 0.0);
    if (n < 0 || n >= (int)sizeof(line_template))
    {
        fprintf(stderr, "Error: snprintf failed.\n");
        return;
    }
    int len = strlen(line_template) + BAR_WIDTH + strlen(COLOR_RESET) + 2;
    int pad = (width - len) / 2;
    if (pad < 0)
        pad = 0;
    if (print_centered("=== CPU Usage & Frequency per Core ===\n\n") == -1)
    {
        fprintf(stderr, "Error: Could not print centered title.\n");
        return;
    }
    // Print table header
    printf("%*s%-7s %-8s %-12s %-s\n", pad, "", "Core", "   Usage", "  Frequency", "                   Load");
    for (int i = 0; i < num_cpus; ++i)
    {
        int cpu_id = cpu_ids[i];
        // Calculate usage deltas
        unsigned long long total1 = user1[i] + nice1[i] + system1[i] + idle1[i];
        unsigned long long total2 = user2[i] + nice2[i] + system2[i] + idle2[i];
        unsigned long long idle_diff = idle2[i] - idle1[i];
        unsigned long long total_diff = total2 - total1;
        float usage = total_diff ? 100.0f * (total_diff - idle_diff) / total_diff : 0.0f;
        // Read current frequency from sysfs
        char path[128], buf[64];
        int npath = snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu_id);
        if (npath < 0 || npath >= (int)sizeof(path))
        {
            fprintf(stderr, "Error: snprintf failed for path.\n");
            continue;
        }
        FILE *fp = fopen(path, "r");
        if (!fp)
        {
            fprintf(stderr, "Error: Could not open %s: %s\n", path, strerror(errno));
            continue;
        }
        float freq_mhz = 0.0f;
        // Read frequency value
        if (fgets(buf, sizeof(buf), fp))
        {
            int freq_khz = atoi(buf);
            freq_mhz = freq_khz / 1000.0f;
        }
        else
        {
            fprintf(stderr, "Error: Could not read frequency from %s: %s\n", path, strerror(errno));
            fclose(fp);
            continue;
        }
        if (fclose(fp) != 0)
        {
            fprintf(stderr, "Error: Could not close %s: %s\n", path, strerror(errno));
        }
        // Prepare the line for this core
        char line[256];
        int nline = snprintf(line, sizeof(line), "CPU %-3d %6.1f%%  %8.2f MHz  ", cpu_id, usage, freq_mhz);
        if (nline < 0 || nline >= (int)sizeof(line))
        {
            fprintf(stderr, "Error: snprintf failed for line.\n");
            continue;
        }
        // Get terminal width for centering
        struct winsize w2;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w2) == -1)
        {
            fprintf(stderr, "Error: Could not get terminal size: %s\n", strerror(errno));
            w2.ws_col = TERM_WIDTH_FALLBACK;
        }
        int width2 = w2.ws_col > 0 ? w2.ws_col : TERM_WIDTH_FALLBACK;
        int len2 = strlen(line) + BAR_WIDTH + strlen(COLOR_RESET) + 2;
        int pad2 = (width2 - len2) / 2;
        // Print the line centered
        if (pad2 > 0)
            printf("%*s%s", pad2, "", line);
        else
            printf("%s", line);
        print_bar(usage);
        printf("\n");
    }
}

// Set terminal to non-canonical mode for non-blocking input
// If enable is 1, set non-blocking mode; if 0, restore previous settings
void set_nonblocking_terminal(int enable)
{
    static struct termios oldt, newt;
    if (enable)
    {
        // Save current terminal settings
        if (tcgetattr(STDIN_FILENO, &oldt) == -1)
        {
            perror("Error: tcgetattr failed");
            exit(EXIT_FAILURE);
        }
        newt = oldt;
        // Disable canonical mode and echo
        newt.c_lflag &= ~(ICANON | ECHO);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == -1)
        {
            perror("Error: tcsetattr failed");
            exit(EXIT_FAILURE);
        }
        // Set non-blocking input
        if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1)
        {
            perror("Error: fcntl (O_NONBLOCK) failed");
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        // Restore old settings
        if (tcsetattr(STDIN_FILENO, TCSANOW, &oldt) == -1)
        {
            perror("Error: tcsetattr (restore) failed");
        }
        if (fcntl(STDIN_FILENO, F_SETFL, 0) == -1)
        {
            perror("Error: fcntl (restore) failed");
        }
    }
}

// Signal handler for SIGINT (Ctrl+C)
// Restores terminal settings and exits the program
void handle_sigint(int sig)
{
    set_nonblocking_terminal(0);
    printf("\nProgram terminated by signal.\n");
    exit(0);
}

// Main function: Entry point of the program
// Sets up signal handler, configures terminal, and runs the main loop
int main()
{
    // Set up signal handler for SIGINT
    if (signal(SIGINT, handle_sigint) == SIG_ERR)
    {
        fprintf(stderr, "Error: Could not set signal handler: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    set_nonblocking_terminal(1);
    int quit = 0;
    while (!quit)
    {
        // Clear screen using ANSI escape codes
        printf("\033[H\033[J");
        // Print CPU usage and frequency for all cores
        print_core_usage_bars();
        // Print quit message centered
        if (print_centered("\nPress 'q' or ESC to quit.\n") == -1)
        {
            fprintf(stderr, "Error: Could not print centered quit message.\n");
            return EXIT_FAILURE;
        }
        // Flush output
        if (fflush(stdout) == EOF)
        {
            perror("Error: fflush failed");
        }
        // Poll for user input every 50ms, up to 1 second
        for (int i = 0; i < 10; ++i)
        {
            int c = getchar();
            if (c == 'q' || c == KEY_ESC)
            {
                quit = 1;
                break;
            }
            if (usleep(TIME_BETWEEN_KEY_POLL_US) != 0)
            {
                fprintf(stderr, "Error: usleep failed: %s\n", strerror(errno));
            }
        }
    }
    // Restore terminal settings
    set_nonblocking_terminal(0);
    // Print exit message centered
    if (print_centered("Version: " VERSION " - Exiting...\n") == -1)
    {
        fprintf(stderr, "Error: Could not print centered exit message.\n");
        return EXIT_FAILURE;
    }
    return 0;
}
