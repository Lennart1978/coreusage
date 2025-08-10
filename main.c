/*
  ____ ___  ____  _____ _   _ ____    _    ____ _____ 
 / ___/ _ \|  _ \| ____| | | / ___|  / \  / ___| ____|
| |  | | | | |_) |  _| | | | \___ \ / _ \| |  _|  _|  
| |__| |_| |  _ <| |___| |_| |___) / ___ \ |_| | |___ 
 \____\___/|_| \_\_____|\___/|____/_/   \_\____|_____|
                                                       
*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include <time.h>
#include <getopt.h>

#define VERSION "1.0.2"
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

static int terminal_modified = 0;
static volatile sig_atomic_t g_should_terminate = 0;
static volatile sig_atomic_t g_winch = 0;

// Runtime-configurable settings
static int g_bar_width = BAR_WIDTH;
static int g_use_color = 1;
static int g_show_temp = 1;
static int g_interval_us = TIME_BETWEEN_SAMPLES_US;

// Helper function: Print a colored progress bar for CPU usage
// Prints a horizontal bar with color depending on the usage percentage.
void print_bar(float percent)
{
    // Calculate how many bar segments should be filled
    int filled = (int)(percent * g_bar_width / 100.0f);
    int tty = isatty(STDOUT_FILENO);
    const char *color;
    // Choose color based on usage percentage
    if (percent < 50)
        color = COLOR_GREEN;
    else if (percent < 80)
        color = COLOR_YELLOW;
    else
        color = COLOR_RED;
    if (g_use_color && tty)
        printf("%s[", color);
    else
        printf("[");
    for (int i = 0; i < g_bar_width; ++i)
    {
        if (i < filled)
            printf("█");
        else
            printf(" ");
    }
    if (g_use_color && tty)
        printf("]%s", COLOR_RESET);
    else
        printf("]");
}

// Helper function: Print a formatted line centered in the terminal
// Uses variable arguments to format the string, then centers it based on terminal width.
int print_centered(const char *fmt, ...)
{
    struct winsize w;
    // Try to get terminal width; fall back silently if not a TTY
    int width;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1)
    {
        width = TERM_WIDTH_FALLBACK;
    }
    else
    {
        width = w.ws_col > 0 ? w.ws_col : TERM_WIDTH_FALLBACK;
    }
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
int read_cpu_stats(unsigned long long user[], unsigned long long nice[], unsigned long long system[], unsigned long long idle[], unsigned long long total[], int cpu_ids[], int *num_cpus)
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
            unsigned long long u = 0, n = 0, s = 0, idl = 0, iw = 0, irq = 0, soft = 0, steal = 0, guest = 0, guest_nice = 0;
            // Parse CPU stats (accept variable field count)
            int matched = sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                                 &cpu_id, &u, &n, &s, &idl, &iw, &irq, &soft, &steal, &guest, &guest_nice);
            if (matched >= 5 && found_cpus < MAX_CPUS)
            {
                cpu_ids[found_cpus] = cpu_id;
                user[found_cpus] = u;
                nice[found_cpus] = n;
                system[found_cpus] = s;
                // Treat idle as idle + iowait when available
                unsigned long long idle_agg = idl + (matched >= 6 ? iw : 0);
                idle[found_cpus] = idle_agg;
                // Total is sum of available fields
                unsigned long long t = u + n + s + idl + (matched >= 6 ? iw : 0) + (matched >= 7 ? irq : 0) +
                                      (matched >= 8 ? soft : 0) + (matched >= 9 ? steal : 0) +
                                      (matched >= 10 ? guest : 0) + (matched >= 11 ? guest_nice : 0);
                total[found_cpus] = t;
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
            unsigned long long u = 0, n = 0, s = 0, idl = 0, iw = 0, irq = 0, soft = 0, steal = 0, guest = 0, guest_nice = 0;
            int matched = sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                                 &cpu_id, &u, &n, &s, &idl, &iw, &irq, &soft, &steal, &guest, &guest_nice);
            if (matched >= 5)
            {
                for (int i = 0; i < *num_cpus; ++i)
                {
                    if (cpu_id == cpu_ids[i])
                    {
                        user[i] = u;
                        nice[i] = n;
                        system[i] = s;
                        unsigned long long idle_agg = idl + (matched >= 6 ? iw : 0);
                        idle[i] = idle_agg;
                        unsigned long long t = u + n + s + idl + (matched >= 6 ? iw : 0) + (matched >= 7 ? irq : 0) +
                                              (matched >= 8 ? soft : 0) + (matched >= 9 ? steal : 0) +
                                              (matched >= 10 ? guest : 0) + (matched >= 11 ? guest_nice : 0);
                        total[i] = t;
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
    unsigned long long user1[MAX_CPUS], nice1[MAX_CPUS], system1[MAX_CPUS], idle1[MAX_CPUS], total1[MAX_CPUS];
    unsigned long long user2[MAX_CPUS], nice2[MAX_CPUS], system2[MAX_CPUS], idle2[MAX_CPUS], total2[MAX_CPUS];
    int cpu_ids[MAX_CPUS];
    int num_cpus = 0;
    // First read: get initial CPU stats and core IDs
    if (read_cpu_stats(user1, nice1, system1, idle1, total1, cpu_ids, &num_cpus) != 0)
    {
        fprintf(stderr, "Error: Could not read CPU statistics.\n");
        return;
    }
    // Wait for the next sample
    struct timespec ts = {0, g_interval_us * 1000};
    if (nanosleep(&ts, NULL) != 0)
    {
        if (errno != EINTR)
            fprintf(stderr, "Error: nanosleep failed: %s\n", strerror(errno));
        // even if interrupted, continue with second read
    }
    // Second read: get updated CPU stats
    if (read_cpu_stats(user2, nice2, system2, idle2, total2, cpu_ids, &num_cpus) != 0)
    {
        fprintf(stderr, "Error: Could not read CPU statistics (second measurement).\n");
        return;
    }
    printf("\n");
    struct winsize w;
    // Get terminal width for centering
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1)
    {
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
    int len = strlen(line_template) + g_bar_width + strlen(COLOR_RESET) + 2;
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
        unsigned long long idle_diff = idle2[i] - idle1[i];
        unsigned long long total_diff = total2[i] - total1[i];
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
        // Compute padding using previously determined terminal width
        int len2 = strlen(line) + g_bar_width + strlen(COLOR_RESET) + 2;
        int pad2 = (width - len2) / 2;
        // Print the line centered
        if (pad2 > 0)
            printf("%*s%s", pad2, "", line);
        else
            printf("%s", line);
        print_bar(usage);
        printf("\n");
    }
}

// Helper function: Prints the CPU temperature
void print_cpu_temperature()
{
    const sensors_chip_name *chip;
    int chip_nr = 0;
    int found = 0;
    double temp_value = 0.0;
    char label_buf[128] = "";
    // Iterate over all chips
    while ((chip = sensors_get_detected_chips(NULL, &chip_nr)) != NULL)
    {
        const sensors_feature *feature;
        int feat_nr = 0;
        // Iterate over all features of the chip
        while ((feature = sensors_get_features(chip, &feat_nr)) != NULL)
        {
            if (feature->type == SENSORS_FEATURE_TEMP)
            {
                const sensors_subfeature *subf = sensors_get_subfeature(chip, feature, SENSORS_SUBFEATURE_TEMP_INPUT);
                if (subf)
                {
                    double value = 0.0;
                    if (sensors_get_value(chip, subf->number, &value) == 0)
                    {
                        // Get label
                        const char *label = sensors_get_label(chip, feature);
                        if (label)
                        {
                            snprintf(label_buf, sizeof(label_buf), "%s", label);
                        }
                        else
                        {
                            snprintf(label_buf, sizeof(label_buf), "Temp");
                        }
                        temp_value = value;
                        found = 1;
                        break;
                    }
                }
            }
        }
        if (found)
            break;
    }
    if (found)
    {
        // Build temperature line exactly like a core line
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1)
            w.ws_col = TERM_WIDTH_FALLBACK;
        int width = w.ws_col > 0 ? w.ws_col : TERM_WIDTH_FALLBACK;
        // Dummy-Label and dummy frequency for harmonic line
        char line[256];
        snprintf(line, sizeof(line), "CPU Temp: %3.1f°C ", temp_value);
        int len = strlen(line) + g_bar_width + strlen(COLOR_RESET) + 2;
        int pad = (width - len) / 2;
        if (pad < 0)
            pad = 0;
        if (pad > 0)
            printf("\n%*s%s", pad, "", line);
        else
            printf("\n%s", line);
        // Normalize temperature to 0-100% (when >100°C, the bar is full)
        float percent = temp_value;
        if (percent < 0)
            percent = 0;
        if (percent > 100)
            percent = 100;
        print_bar(percent);
        printf("\n");
    }
    else
    {
        print_centered("CPU temperature: not available\n");
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
            exit(EXIT_FAILURE);
        }
        if (fcntl(STDIN_FILENO, F_SETFL, 0) == -1)
        {
            perror("Error: fcntl (restore) failed");
            exit(EXIT_FAILURE);
        }
    }
}

static void restore_terminal(void)
{
    if (terminal_modified && isatty(STDIN_FILENO))
    {
        set_nonblocking_terminal(0);
        terminal_modified = 0;
    }
}

static void signal_handler(int sig)
{
    if (sig == SIGWINCH)
    {
        g_winch = 1;
        return;
    }
    g_should_terminate = 1;
}

static int setup_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) == -1)
        return -1;
    if (sigaction(SIGHUP, &sa, NULL) == -1)
        return -1;
    if (sigaction(SIGWINCH, &sa, NULL) == -1)
        return -1;
    return 0;
}

// Main function: Entry point of the program
// Sets up signal handler, configures terminal, and runs the main loop
int main(int argc, char **argv)
{
    // Parse CLI options
    static struct option long_opts[] = {
        {"interval", required_argument, 0, 'i'},
        {"bar-width", required_argument, 0, 'w'},
        {"no-color", no_argument, 0, 'c'},
        {"no-temp", no_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 'i':
        {
            long ms = strtol(optarg, NULL, 10);
            if (ms > 0 && ms < 60000)
                g_interval_us = (int)ms * 1000;
            break;
        }
        case 'w':
        {
            long w = strtol(optarg, NULL, 10);
            if (w >= 5 && w <= 200)
                g_bar_width = (int)w;
            break;
        }
        case 'c':
            g_use_color = 0;
            break;
        case 't':
            g_show_temp = 0;
            break;
        case 'h':
        default:
            printf("coreusage v.%s\n", VERSION);
            printf("Options:\n");
            printf("  --interval <ms>   Sample interval (default %d ms)\n", TIME_BETWEEN_SAMPLES_US / 1000);
            printf("  --bar-width <n>   Width of the bar (default %d)\n", BAR_WIDTH);
            printf("  --no-color        Disable ANSI colors\n");
            printf("  --no-temp         Hide temperature line\n");
            return 0;
        }
    }
    // Set up signal handlers and terminal cleanup
    if (setup_signal_handlers() == -1)
    {
        fprintf(stderr, "Error: Could not set signal handlers: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    atexit(restore_terminal);
    if (isatty(STDIN_FILENO))
    {
        set_nonblocking_terminal(1);
        terminal_modified = 1;
    }
    // Initialize libsensors once
    if (sensors_init(NULL) != 0)
    {
        fprintf(stderr, "Error: Could not initialize libsensors: %s\n", sensors_strerror(errno));
        return EXIT_FAILURE;
    }
    int quit = 0;
    while (!quit)
    {
        if (g_should_terminate)
        {
            quit = 1;
            break;
        }
        // If terminal size changed, just clear and continue
        if (g_winch)
        {
            g_winch = 0;
        }
        // Clear screen using ANSI escape codes
        printf("\033[H\033[J");
        // Print CPU usage and frequency for all cores
        print_core_usage_bars();
        // Print CPU temperature (optional)
        if (g_show_temp)
            print_cpu_temperature();
        // Print quit message centered
        if (print_centered("\nPress 'q' or ESC to quit.\n") == -1)
        {
            fprintf(stderr, "Error: Could not print centered quit message.\n");
            return EXIT_FAILURE;
        }
        // Flush output before entering input polling (important for non-tty stdout)
        if (fflush(stdout) == EOF)
        {
            perror("Error: fflush failed");
        }
        // Poll for user input every 50ms, up to 1 second
        for (int i = 0; i < 10; ++i)
        {
            if (g_should_terminate)
            {
                quit = 1;
                break;
            }
            if (isatty(STDIN_FILENO))
            {
                int c = getchar();
                if (c == 'q' || c == KEY_ESC)
                {
                    quit = 1;
                    break;
                }
            }
            struct timespec ts_poll = {0, TIME_BETWEEN_KEY_POLL_US * 1000};
            if (nanosleep(&ts_poll, NULL) != 0)
            {
                if (errno != EINTR)
                    fprintf(stderr, "Error: nanosleep failed: %s\n", strerror(errno));
            }
        }
    }
    // Restore terminal settings
    if (terminal_modified && isatty(STDIN_FILENO))
    {
        set_nonblocking_terminal(0);
        terminal_modified = 0;
    }
    // Print exit message centered
    if (print_centered("coreusage v." VERSION " - libsensors v.%s - Exiting...\n", libsensors_version != NULL ? libsensors_version : "unknown") == -1)
    {
        fprintf(stderr, "Error: Could not print centered exit message.\n");
        return EXIT_FAILURE;
    }
    sensors_cleanup();
    return 0;
}
