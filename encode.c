/**
 * Attempts to decode speech data extracted from a North American phone.
 *
 * LICENSE: public domain.
 * (c) 2015, Philippe Michaud-Boudreault
 */
#include "upd.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <sndfile.h>
#include <signal.h>

extern const int upd7759_step[16][16];
extern const int upd7759_state_table[16];

/*
   We know from the datasheet that this chip has an internal 9-bit DAC.
   What the hell was NEC thinking, choosing such an odd bit-depth? In any
   case, this is the first software-based encoder in existence to support 
   the chip. We can only suspect that the hardware encoder was made by
   stoned wizards. Subsequently, we've only hired stoned wizards to
   assist us in this undertaking.
 */
static struct {
    char *inputFileName, *outputFileName;
    uint8_t verbose;
} options = {0,};

typedef struct {
    SNDFILE *fp;
    SF_INFO info;
    short *data;
    uint64_t data_size;
} input_file_t;

typedef enum {
    NONE = 0x0 << 3,
    FIVEKHZ = 0x5f,
    SIXKHZ = 0x59,
    EIGHTKHZ = 0x53
} frequency_upd7759_t;

frequency_upd7759_t frequency_upd7759 = NONE;

static void cleanup();

static void kill_str(char *errstr, uint8_t exit_status) {
    fputs("Sorry :(", stderr);
    if (errstr)
        fprintf(stderr, " -- %s", errstr);
    fputc('\n', stderr);
    cleanup();
    exit(exit_status);
}

static void kill_errno(int error, uint8_t exit_status) {
    if (!error)
        kill_str(NULL, exit_status);
    kill_str(strerror(error), exit_status);
}

#define kill(a) kill_errno(0, a)

static void process_arguments(int argc, char **argv) {
    char c;

    while ((c = getopt(argc, argv, "i:o:v")) != -1) {
        if (c == 'i') 
            options.inputFileName = strdup(optarg);
        else if (c == 'o')
            options.outputFileName = strdup(optarg);
        else if (c == 'v')
            options.verbose = 1;
    }
}

static void cleanup() {
    if (options.inputFileName)
        free(options.inputFileName);
    if (options.outputFileName)
        free(options.outputFileName);
}

// XXX: Stack memory allocated for returned object.
//	MUST be free'd after it's done.
static input_file_t *open_input() {
    input_file_t *input_file = calloc(1, sizeof(*input_file));

    if (!input_file)
        kill_errno(errno, EXIT_FAILURE);

    if (options.inputFileName) {
        input_file->fp = sf_open(options.inputFileName, SFM_READ, &input_file->info);
    } else {
        input_file->fp = sf_open_fd(STDIN_FILENO, SFM_READ, &input_file->info, 0);
    }

    if (!input_file->fp) {
        char err[256] = {0,};
        sf_error_str(input_file->fp, err, sizeof(err));
        kill_str(err, EXIT_FAILURE);
    }

    return input_file;
}

static FILE *open_output() {
    FILE *fp = NULL;

    if (options.outputFileName) {
        fp = fopen(options.outputFileName, "w");
    } else {
        fp = fdopen(STDOUT_FILENO, "w");
    }

    if (!fp)
        kill_errno(errno, EXIT_FAILURE);

    return fp;
}

static input_file_t *input_file = NULL;

static void read_pcm_file() {
    sf_count_t ret = 0, pos = 0;

    input_file = open_input();

    if ((input_file->info.samplerate != 5000) 
        && (input_file->info.samplerate != 6000)
        && (input_file->info.samplerate != 8000)) {
        kill_str(
            "Only sample rates of 5khz, 6khz, or 8kz are supported.",
            EXIT_FAILURE
        );
    }

    if (input_file->info.channels != 1) {
        kill_str(
            "Only single channel audio is supported.",
            EXIT_FAILURE
        );
    }

    if (!(input_file->info.format & SF_FORMAT_PCM_16)) {
        kill_str(
            "Audio data must be 16-bit PCM.",
            EXIT_FAILURE
        );
    }

    if (options.verbose) {
        printf("Frames:         %ld\n", (int64_t)input_file->info.frames);
        printf("Sample Rate:    %d\n", input_file->info.samplerate);
        printf("Channels:       %d\n", input_file->info.channels);
        printf("Format:         0x%X\n", input_file->info.format);
        printf("Sections:       %d\n", input_file->info.sections);
        printf("Seekable:       %d\n", input_file->info.seekable);
    }

    input_file->data_size = (uint64_t)input_file->info.frames;
    input_file->data = malloc(input_file->data_size * sizeof(*input_file->data));

    sf_readf_short(input_file->fp, input_file->data, input_file->info.frames);
    sf_close(input_file->fp);

    free(input_file);
}

static uint8_t get_frequency(int samplerate) {
    if (samplerate == 5000)
        return FIVEKHZ;
    if (samplerate == 6000)
        return SIXKHZ;
    if (samplerate == 8000)
        return EIGHTKHZ;
    return 0;
}

static void output_upd_file() {
    FILE *o = open_output();
    uint32_t i;
    int state = 0, onesixty = 0;
    char sample;
    uint8_t output_sample = 0x0;
    uint8_t freq = get_frequency(input_file->info.samplerate);

    fwrite(&freq, sizeof(freq), 1, o);

    for (i = 0; i < input_file->data_size; ++i) {
        uint16_t sample2 = input_file->data[i];
        sample = (sample2 >> 7);
        if (state < 0) state = 0;
        else if (state > 15) state = 15;
        if (sample < 0) sample = 0;
        else if (sample > 15) sample = 15;
      //  state %= 16;
       // sample %= 16;
      //  state -= upd7759_state_table[sample];
        state = (upd7759_state_table[sample] - state);
#ifdef DEBUG
        fprintf(stderr, "State%.04x\n", state);
#endif
      //  sample -= upd7759_step[state][sample];
        sample = (upd7759_step[state][sample] - sample);
#ifdef DEBUG
        fprintf(stderr, "Sample%.02x\n", sample);
#endif
        if (onesixty & 1) {
            output_sample = (output_sample << 4) | (sample & 0x0f);
            fwrite(&output_sample, sizeof(output_sample), 1, o);
        } else {
            output_sample = sample & 0x0f;
        }

        if (++onesixty == 256) {
            onesixty = 0;
            fwrite(&freq, sizeof(freq), 1, o);
        }
    }

    if (onesixty & 1) { // purge remaining data
        output_sample <<= 4;
        fwrite(&output_sample, sizeof(output_sample), 1, o);
    }

    fclose(o);
}

int main(int argc, char *argv[]) {
    process_arguments(argc, argv);
    read_pcm_file();
    output_upd_file();
    cleanup();

    return EXIT_SUCCESS;
}
