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

#define MAX_CPUS 128 // Maximum number of CPU cores supported
#define BAR_WIDTH 40 // Width of the usage bar
#define COLOR_RESET "\033[0m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED "\033[31m"

// Helper function: Print a colored progress bar for CPU usage
void print_bar(float percent)
{
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
void print_centered(const char *fmt, ...)
{
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);     // Get terminal size
    int width = w.ws_col > 0 ? w.ws_col : 80; // Default to 80 if detection fails
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    int len = strlen(buf);
    int pad = (width - len) / 2;
    if (pad > 0)
        printf("%*s%s", pad, "", buf); // Print padding and then the string
    else
        printf("%s", buf);
}

// Reads CPU usage statistics per core from /proc/stat
// If cpu_ids is NULL or *num_cpus == 0, it initializes cpu_ids and returns the number of CPUs
// Otherwise, it updates the usage values for the given cpu_ids
int read_cpu_stats(unsigned long long user[], unsigned long long nice[], unsigned long long system[], unsigned long long idle[], int cpu_ids[], int *num_cpus)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp)
        return -1;
    char line[256];
    if (cpu_ids == NULL || *num_cpus == 0)
    {
        int found_cpus = 0;
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
    fclose(fp);
    return 0;
}

// For each core, print frequency, usage and progress bar in one centered line
void print_core_usage_bars()
{
    unsigned long long user1[MAX_CPUS], nice1[MAX_CPUS], system1[MAX_CPUS], idle1[MAX_CPUS];
    unsigned long long user2[MAX_CPUS], nice2[MAX_CPUS], system2[MAX_CPUS], idle2[MAX_CPUS];
    int cpu_ids[MAX_CPUS];
    int num_cpus = 0;
    // First read: get initial CPU stats and core IDs
    if (read_cpu_stats(user1, nice1, system1, idle1, cpu_ids, &num_cpus) != 0)
    {
        printf("Error reading /proc/stat\n");
        return;
    }
    usleep(200000); // Wait 200 ms for next sample
    // Second read: get updated CPU stats
    read_cpu_stats(user2, nice2, system2, idle2, cpu_ids, &num_cpus);
    printf("\n");
    // Berechne das Padding wie bei den Core-Zeilen
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int width = w.ws_col > 0 ? w.ws_col : 80;
    char line_template[256];
    snprintf(line_template, sizeof(line_template), "CPU %-3d %6.1f%%  %8.2f MHz  ", 0, 0.0, 0.0);
    int len = strlen(line_template) + BAR_WIDTH + strlen(COLOR_RESET) + 2; // +2 für []
    int pad = (width - len) / 2;
    if (pad < 0)
        pad = 0;
    // Überschrift mittig ausgeben
    print_centered("=== CPU Usage & Frequency per Core ===\n\n");
    // Spaltenüberschrift mit Padding wie bisher
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
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu_id);
        FILE *fp = fopen(path, "r");
        float freq_mhz = 0.0f;
        if (fp && fgets(buf, sizeof(buf), fp))
        {
            int freq_khz = atoi(buf);
            freq_mhz = freq_khz / 1000.0f;
        }
        if (fp)
            fclose(fp);
        // Prepare the line for this core
        char line[256];
        snprintf(line, sizeof(line), "CPU %-3d %6.1f%%  %8.2f MHz  ", cpu_id, usage, freq_mhz);
        // Center the line with the bar
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        int width = w.ws_col > 0 ? w.ws_col : 80;
        int len = strlen(line) + BAR_WIDTH + strlen(COLOR_RESET) + 2; // +2 for []
        int pad = (width - len) / 2;
        if (pad > 0)
            printf("%*s%s", pad, "", line);
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
        tcgetattr(STDIN_FILENO, &oldt); // Save current terminal settings
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);         // Disable canonical mode and echo
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);  // Apply new settings
        fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK); // Set non-blocking input
    }
    else
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restore old settings
        fcntl(STDIN_FILENO, F_SETFL, 0);         // Restore blocking input
    }
}

int main()
{
    set_nonblocking_terminal(1); // Enable non-blocking terminal input
    int quit = 0;
    while (!quit)
    {
        printf("\033[H\033[J");                          // Clear screen using ANSI escape codes
        print_core_usage_bars();                         // Print CPU usage and frequency for all cores
        print_centered("\nPress 'q' or ESC to quit.\n"); // Print quit instruction centered
        fflush(stdout);
        // Poll for user input every 50ms, up to 1 second
        for (int i = 0; i < 10; ++i)
        {
            int c = getchar();
            if (c == 'q' || c == 27)
            { // 27 = ESC
                quit = 1;
                break;
            }
            usleep(50000);
        }
    }
    set_nonblocking_terminal(0);    // Restore terminal settings
    print_centered("Exiting...\n"); // Print exit message centered
    return 0;
}
