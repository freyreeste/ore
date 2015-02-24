#include <R.h>
#include <Rdefines.h>
#include <Rinternals.h>

#include <string.h>

#include "compile.h"
#include "match.h"
#include "print.h"

extern UChar * onigenc_step_back (OnigEncoding enc, const UChar *start, const UChar *s, int n);

// Extract an element of an R list by name
SEXP ore_get_list_element (SEXP list, const char *name)
{
    SEXP element = R_NilValue;
    SEXP names = getAttrib(list, R_NamesSymbol);
    
    for (int i=0; i<length(names); i++)
    {
    	if (strcmp(CHAR(STRING_ELT(names,i)), name) == 0)
        {
    	   element = VECTOR_ELT(list, i);
    	   break;
       }
    }
    
    return element;
}

// Create a printstate_t object, which keeps track of various things
printstate_t * ore_alloc_printstate (const int context, const int width, const int max_lines, const Rboolean use_colour, const int n_matches, const int max_enc_len)
{
    printstate_t *state = (printstate_t *) R_alloc(1, sizeof(printstate_t));
    
    // Store simple values directly
    state->use_colour = use_colour;
    state->max_lines = max_lines;
    state->n_matches = n_matches;
    
    // If context or number lines will be needed, we remove part of the width for the initial labels
    if (use_colour && n_matches == 1)
        state->width = width;
    else
        state->width = width - 9;
    
    // Initialisations
    state->in_match = FALSE;
    state->loc = 0;
    state->current_match = 0;
    state->lines_done = 0;
    
    // If we're using colour we need to allocate enough space for the nine control bytes either side of each match
    if (use_colour)
    {
        state->match = R_alloc(max_enc_len+9, width);
        state->context = NULL;
    }
    else
    {
        state->match = R_alloc(max_enc_len, width);
        state->context = R_alloc(max_enc_len, width);
    }
    
    // If there is more than one match, allocate memory for the number line
    if (n_matches == 1)
        state->number = NULL;
    else
        state->number = R_alloc(1, width);
    
    // Pointers to the start of each line
    state->match_start = state->match;
    state->context_start = state->context;
    state->number_start = state->number;
    
    return state;
}

// Can more lines be printed, or should we stop?
Rboolean ore_more_lines (printstate_t *state)
{
    return (state->lines_done < state->max_lines);
}

// This function actually prints a line of buffered text, with annotations, to the terminal
void ore_print_line (printstate_t *state)
{
    // Forget it if the buffer is empty, or we're already printed as many lines as are allowed
    if (state->loc == 0 || state->lines_done >= state->max_lines)
        return;
    
    // Switch off colour printing temporarily if we're in the middle of a match
    if (state->use_colour && state->in_match)
    {
        strncpy(state->match, "\x1b[0m", 4);
        state->match += 4;
    }
    *state->match = '\0';
    
    // Print out the match string, alone or with a label
    if (state->use_colour && state->n_matches == 1)
        Rprintf("%s\n", state->match_start);
    else
        Rprintf("  match: %s\n", state->match_start);
    
    // Print the context, if it's a separate line (i.e. if we're not using colour)
    if (!state->use_colour)
    {
        *state->context = '\0';
        Rprintf("context: %s\n", state->context_start);
    }
    
    // Print numbers if there is more than one match
    if (state->n_matches > 1)
    {
        *state->number = '\0';
        Rprintf(" number: %s\n", state->number_start);
    }
    
    Rprintf("\n");
    
    // Reset
    state->match = state->match_start;
    state->context = state->context_start;
    state->number = state->number_start;
    state->loc = 0;
    
    // Turn colour back on if we were mid-match
    if (state->use_colour && state->in_match)
    {
        strncpy(state->match, "\x1b[36m", 5);
        state->match += 5;
    }
    
    // Keep count of lines done
    state->lines_done++;
}

// This function adds a byte to the match (or context), updating other lines appropriately
void ore_do_push_byte (printstate_t *state, const char byte, Rboolean zero_width)
{
    if (state->in_match || state->use_colour)
    {
        *(state->match++) = byte;
        if (!state->use_colour && !zero_width)
            *(state->context++) = ' ';
        if (state->n_matches > 1 && !zero_width)
        {
            if (state->in_match)
            {
                // Print the next digit of the number, or '=' if we've finished them
                if (*state->current_match_loc == '\0')
                    *(state->number++) = '=';
                else
                    *(state->number++) = *(state->current_match_loc++);
            }
            else
                *(state->number++) = ' ';
        }
    }
    else
    {
        *(state->context++) = byte;
        if (!state->use_colour && !zero_width)
            *(state->match++) = ' ';
        if (state->n_matches > 1 && !zero_width)
            *(state->number++) = ' ';
    }
}

// Switch from inside to outside match state, or vice versa
void ore_switch_state (printstate_t *state, Rboolean match)
{
    if (match && !state->in_match)
    {
        // Append the colour escape code, if appropriate
        if (state->use_colour)
        {
            strncpy(state->match, "\x1b[36m", 5);
            state->match += 5;
        }
        
        // Find current match number and convert to string
        state->current_match++;
        if (state->current_match < 100000)
            sprintf(state->current_match_string, "%d", state->current_match);
        else
            state->current_match_string[0] = '\0';
        state->current_match_loc = state->current_match_string;
        
        state->in_match = TRUE;
    }
    else if (!match && state->in_match)
    {
        if (state->use_colour)
        {
            strncpy(state->match, "\x1b[0m", 4);
            state->match += 4;
        }
        
        state->in_match = FALSE;
    }
}

void ore_push_byte (printstate_t *state, const char byte, int width)
{
    if (width < 0)
    {
        switch (byte)
        {
            case '\t':
            case '\n':
            width = 2;
            break;
        
            default:
            width = 1;
        }
    }
    
    if (state->loc + width >= state->width)
        ore_print_line(state);
    
    switch (byte)
    {
        case '\t':
        ore_do_push_byte(state, '\\', FALSE);
        ore_do_push_byte(state, 't', FALSE);
        break;
        
        case '\n':
        ore_do_push_byte(state, '\\', FALSE);
        ore_do_push_byte(state, 'n', FALSE);
        break;
        
        default:
        ore_do_push_byte(state, byte, width==0);
    }
    
    state->loc += width;
}

UChar * ore_push_chars (printstate_t *state, UChar *ptr, int n, OnigEncoding encoding)
{
    for (int i=0; i<n; i++)
    {
        int char_len = encoding->mbc_enc_len(ptr);
        ore_push_byte(state, *(ptr++), -1);
        for (int k=1; k<char_len; k++)
            ore_push_byte(state, *(ptr++), 0);
    }
    
    return ptr;
}

SEXP ore_print_match (SEXP match, SEXP context_, SEXP width_, SEXP max_lines_, SEXP use_colour_)
{
    const int context = asInteger(context_);
    const int width = asInteger(width_);
    const int max_lines = asInteger(max_lines_);
    const Rboolean use_colour = (asLogical(use_colour_) == TRUE);
    
    const int n_matches = asInteger(ore_get_list_element(match, "nMatches"));
    
    SEXP text_ = ore_get_list_element(match, "text");
    const UChar *text = (const UChar *) CHAR(STRING_ELT(text_, 0));
    OnigEncoding encoding = ore_r_to_onig_enc(getCharCE(STRING_ELT(text_, 0)));
    size_t text_len = onigenc_strlen_null(encoding, text);
    
    const int *offsets_ = (const int *) INTEGER(ore_get_list_element(match, "offsets"));
    const int *byte_offsets_ = (const int *) INTEGER(ore_get_list_element(match, "byteOffsets"));
    int *offsets = (int *) R_alloc(n_matches, sizeof(int));
    int *byte_offsets = (int *) R_alloc(n_matches, sizeof(int));
    for (int i=0; i<n_matches; i++)
    {
        offsets[i] = offsets_[i] - 1;
        byte_offsets[i] = byte_offsets_[i] - 1;
    }
    
    const int *lengths = (const int *) INTEGER(ore_get_list_element(match, "lengths"));
    const int *byte_lengths = (const int *) INTEGER(ore_get_list_element(match, "byteLengths"));
    
    printstate_t *state = ore_alloc_printstate(context, width, max_lines, use_colour, n_matches, encoding->max_enc_len);
    
    size_t start = 0;
    Rboolean reached_end = FALSE;
    for (int i=0; i<n_matches; i++)
    {
        int precontext_len = 0, postcontext_len = 0;
        UChar *ptr;
        
        if (offsets[i] - start > context)
        {
            precontext_len = context;
            ptr = onigenc_step_back(encoding, text, text+byte_offsets[i], precontext_len);
            for (int j=0; j<3; j++)
                ore_push_byte(state, '.', 1);
        }
        else if (offsets[i] > start)
        {
            precontext_len = offsets[i] - start;
            ptr = onigenc_step_back(encoding, text, text+byte_offsets[i], precontext_len);
        }
        else
            ptr = (UChar *) text + byte_offsets[i];
        
        ptr = ore_push_chars(state, ptr, precontext_len, encoding);
        ore_switch_state(state, TRUE);
        ptr = ore_push_chars(state, ptr, lengths[i], encoding);
        ore_switch_state(state, FALSE);
        
        start = offsets[i] + lengths[i];
        
        if (i == n_matches - 1)
        {
            if (text_len - start <= context)
            {
                postcontext_len = text_len - start;
                reached_end = TRUE;
            }
            else
                postcontext_len = context;
        }
        else if (offsets[i+1] - start > context)
        {
            if (offsets[i+1] - start - context <= context)
                postcontext_len = offsets[i+1] - start - context;
            else
                postcontext_len = context;
        }
        
        ptr = ore_push_chars(state, ptr, postcontext_len, encoding);
        
        start += postcontext_len;
        
        if (!ore_more_lines(state))
        {
            reached_end = TRUE;
            break;
        }
    }
    
    if (!reached_end)
    {
        for (int j=0; j<3; j++)
            ore_push_byte(state, '.', 1);
    }
    
    ore_print_line(state);
    
    return R_NilValue;
}