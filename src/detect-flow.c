/**
 * \file
 * \author Victor Julien <victor@inliniac.net>
 *
 * FLOW part of the detection engine.
 */

#include <pcre.h>

#include "debug.h"
#include "decode.h"
#include "detect.h"

#include "flow.h"
#include "flow-var.h"

#include "detect-flow.h"

#include "util-unittest.h"

/**
 * \brief Regex for parsing our flow options
 */
#define PARSE_REGEX  "^\\s*([A-z_]+)\\s*(?:,\\s*([A-z_]+))?\\s*(?:,\\s*([A-z_]+))?\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectFlowMatch (ThreadVars *, PatternMatcherThread *, Packet *, Signature *, SigMatch *);
int DetectFlowSetup (DetectEngineCtx *, Signature *, SigMatch *, char *);
void DetectFlowRegisterTests(void);
void DetectFlowFree(DetectFlowData *);

/**
 * \brief Registration function for flow: keyword
 * \todo add support for no_stream and stream_only
 */
void DetectFlowRegister (void) {
    sigmatch_table[DETECT_FLOW].name = "flow";
    sigmatch_table[DETECT_FLOW].Match = DetectFlowMatch;
    sigmatch_table[DETECT_FLOW].Setup = DetectFlowSetup;
    sigmatch_table[DETECT_FLOW].Free  = NULL;
    sigmatch_table[DETECT_FLOW].RegisterTests = DetectFlowRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        printf("pcre compile of \"%s\" failed at offset %d: %s\n", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        printf("pcre study failed: %s\n", eb);
        goto error;
    }
    return;

error:
    /* XXX */
    return;
}

/*
 * returns 0: no match
 *         1: match
 *        -1: error
 */

/**
 * \brief This function is used to match flow flags set on a packet with those passed via flow:
 * \todo We need to add support for no_stream and stream_only flag checking
 *
 * \param t pointer to thread vars
 * \param pmt pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into DetectFlowData
 *
 * \retval 0 no match
 * \retval 1 match
 */
int DetectFlowMatch (ThreadVars *t, PatternMatcherThread *pmt, Packet *p, Signature *s, SigMatch *m)
{
    u_int8_t cnt = 0;
    DetectFlowData *fd = (DetectFlowData *)m->ctx;

    if (fd->flags & FLOW_PKT_TOSERVER && p->flowflags & FLOW_PKT_TOSERVER) {
        cnt++;
    } else if (fd->flags & FLOW_PKT_TOCLIENT && p->flowflags & FLOW_PKT_TOCLIENT) {
        cnt++;
    }

    if (fd->flags & FLOW_PKT_ESTABLISHED && p->flowflags & FLOW_PKT_ESTABLISHED) {
        cnt++;
    } else if (fd->flags & FLOW_PKT_STATELESS) {
        cnt++;
    }

    int ret = (fd->match_cnt == cnt) ? 1 : 0;
    //printf("DetectFlowMatch: returning %d cnt %d fd->match_cnt %d fd->flags 0x%02X p->flowflags 0x%02X \n", ret, cnt,
              //fd->match_cnt, fd->flags, p->flowflags);
    return ret;
}

/**
 * \brief This function is used to parse flow options passed via flow: keyword
 *
 * \param flowstr Pointer to the user provided flow options
 *
 * \retval fd pointer to DetectFlowData on success
 * \retval NULL on failure
 */
DetectFlowData *DetectFlowParse (char *flowstr)
{
    DetectFlowData *fd = NULL;
    char *args[3] = {NULL,NULL,NULL};
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(parse_regex, parse_regex_study, flowstr, strlen(flowstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret < 1 || ret > 4) {
        //printf("DetectFlowParse: parse error, ret %d, string %s\n", ret, flowstr);
        goto error;
    }
    if (ret > 1) {
        const char *str_ptr;
        res = pcre_get_substring((char *)flowstr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
        if (res < 0) {
            printf("DetectFlowParse: pcre_get_substring failed\n");
            goto error;
        }
        args[0] = (char *)str_ptr;

        if (ret > 2) {
            res = pcre_get_substring((char *)flowstr, ov, MAX_SUBSTRINGS, 2, &str_ptr);
            if (res < 0) {
                printf("DetectFlowParse: pcre_get_substring failed\n");
                goto error;
            }
            args[1] = (char *)str_ptr;
        }
        if (ret > 3) {
            res = pcre_get_substring((char *)flowstr, ov, MAX_SUBSTRINGS, 3, &str_ptr);
            if (res < 0) {
                printf("DetectFlowParse: pcre_get_substring failed\n");
                goto error;
            }
            args[2] = (char *)str_ptr;
        }
    }

    fd = malloc(sizeof(DetectFlowData));
    if (fd == NULL) {
        printf("DetectFlowParse malloc failed\n");
        goto error;
    }
    fd->flags = 0;
    fd->match_cnt = 0;

    int i;
    for (i = 0; i < (ret -1); i++) {
        if (args[i]) {
            /* inspect our options and set the flags */
            if (strcmp(args[i], "established") == 0) {
                if (fd->flags & FLOW_PKT_ESTABLISHED) {
                    //printf("DetectFlowParse error FLOW_PKT_ESTABLISHED flag is already set \n");
                    goto error;
                } else if (fd->flags & FLOW_PKT_STATELESS) {
                    //printf("DetectFlowParse error cannot set established, FLOW_PKT_STATELESS already set \n");
                    goto error;
                }
                fd->flags |= FLOW_PKT_ESTABLISHED;
            } else if (strcmp(args[i], "stateless") == 0) {
                if (fd->flags & FLOW_PKT_STATELESS) {
                    //printf("DetectFlowParse error FLOW_PKT_STATELESS flag is already set \n");
                    goto error;
                } else if (fd->flags & FLOW_PKT_ESTABLISHED) {
                    //printf("DetectFlowParse error cannot set FLOW_PKT_STATELESS, FLOW_PKT_ESTABLISHED already set\n");
                    goto error;
                }
                fd->flags |= FLOW_PKT_STATELESS;
            } else if (strcmp(args[i], "to_client") == 0 || strcmp(args[i], "from_server") == 0) {
                if (fd->flags & FLOW_PKT_TOCLIENT) {
                    //printf("DetectFlowParse error cannot set FLOW_PKT_TOCLIENT flag is already set\n");
                    goto error;
                } else if (fd->flags & FLOW_PKT_TOSERVER) {
                    //printf("DetectFlowParse error cannot set to_client, FLOW_PKT_TOSERVER already set\n");
                    goto error;
                }
                fd->flags |= FLOW_PKT_TOCLIENT;
            } else if (strcmp(args[i], "to_server") == 0 || strcmp(args[i], "from_client") == 0){
                if (fd->flags & FLOW_PKT_TOSERVER) {
                    //printf("DetectFlowParse error cannot set FLOW_PKT_TOSERVER flag is already set\n");
                    goto error;
                } else if (fd->flags & FLOW_PKT_TOCLIENT) {
                    //printf("DetectFlowParse error cannot set to_server, FLOW_PKT_TO_CLIENT flag already set\n");
                    goto error;
                }
                fd->flags |= FLOW_PKT_TOSERVER;
            } else if (strcmp(args[i], "stream_only") == 0) {
                if (fd->flags & FLOW_PKT_STREAMONLY) {
                    //printf("DetectFlowParse error cannot set stream_only flag is already set \n");
                    goto error;
                } else if (fd->flags & FLOW_PKT_NOSTREAM) {
                    //printf("DetectFlowParse error cannot set stream_only flag, FLOW_PKT_NOSTREAM already set\n");
                    goto error;
                }
                fd->flags |= FLOW_PKT_STREAMONLY;
            } else if (strcmp(args[i], "no_stream") == 0) {
                if (fd->flags & FLOW_PKT_NOSTREAM) {
                    //printf("DetectFlowParse error cannot set no_stream flag is already set \n");
                    goto error;
                } else if (fd->flags & FLOW_PKT_STREAMONLY) {
                    //printf("DetectFlowParse error cannot set no_stream flag, FLOW_PKT_STREAMONLY already set\n");
                    goto error;
                }
                fd->flags |= FLOW_PKT_NOSTREAM;
            } else {
                //printf("invalid flow option %s\n",args[i]);
                goto error;
            }

            fd->match_cnt++;
            //printf("args[%d]: %s match_cnt: %d flags: 0x%02X\n", i, args[i], fd->match_cnt, fd->flags);
        }
    }
    for (i = 0; i < (ret -1); i++){
        if (args[i] != NULL) free(args[i]);
    }
    return fd;

error:
    for (i = 0; i < (ret -1); i++){
        if (args[i] != NULL) free(args[i]);
    }
    if (fd != NULL) DetectFlowFree(fd);
    return NULL;

}

/**
 * \brief this function is used to add the parsed flowdata into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param m pointer to the Current SigMatch
 * \param flowstr pointer to the user provided flow options
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int DetectFlowSetup (DetectEngineCtx *de_ctx, Signature *s, SigMatch *m, char *flowstr)
{
    DetectFlowData *fd = NULL;
    SigMatch *sm = NULL;

    //printf("DetectFlowSetup: \'%s\'\n", flowstr);

    fd = DetectFlowParse(flowstr);
    if (fd == NULL) goto error;

    /* Okay so far so good, lets get this into a SigMatch
     * and put it in the Signature. */
    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_FLOW;
    sm->ctx = (void *)fd;

    SigMatchAppend(s,m,sm);

    return 0;

error:
    if (fd != NULL) DetectFlowFree(fd);
    if (sm != NULL) free(sm);
    return -1;

}

/**
 * \brief this function will free memory associated with DetectFlowData
 *
 * \param fd pointer to DetectFlowData
 */
void DetectFlowFree(DetectFlowData *fd) {
    free(fd);
}

/**
 * \test DetectFlowTestParse01 is a test to make sure that we return "something"
 *  when given valid flow opt
 */
int DetectFlowTestParse01 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("established");
    if (fd != NULL) {
        DetectFlowFree(fd);
        result = 1;
    }

    return result;
}

/**
 * \test DetectFlowTestParse02 is a test for setting the established flow opt
 */
int DetectFlowTestParse02 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("established");
    if (fd != NULL) {
        if (fd->flags == FLOW_PKT_ESTABLISHED && fd->match_cnt == 1) {
            result = 1;
        } else {
            printf("expected 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_ESTABLISHED, 1, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse03 is a test for setting the stateless flow opt
 */
int DetectFlowTestParse03 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("stateless");
    if (fd != NULL) {
        if (fd->flags == FLOW_PKT_STATELESS && fd->match_cnt == 1) {
            result = 1;
        } else {
            printf("expected 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_STATELESS, 1, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse04 is a test for setting the to_client flow opt
 */
int DetectFlowTestParse04 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("to_client");
    if (fd != NULL) {
        if (fd->flags == FLOW_PKT_TOCLIENT && fd->match_cnt == 1) {
            result = 1;
        } else {
            printf("expected 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_TOCLIENT, 1, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse05 is a test for setting the to_server flow opt
 */
int DetectFlowTestParse05 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("to_server");
    if (fd != NULL) {
        if (fd->flags == FLOW_PKT_TOSERVER && fd->match_cnt == 1) {
            result = 1;
        } else {
            printf("expected 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_TOSERVER, 1, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse06 is a test for setting the from_server flow opt
 */
int DetectFlowTestParse06 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("from_server");
    if (fd != NULL) {
        if (fd->flags == FLOW_PKT_TOCLIENT && fd->match_cnt == 1) {
            result = 1;
        } else {
            printf("expected 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_TOCLIENT, 1, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse07 is a test for setting the from_client flow opt
 */
int DetectFlowTestParse07 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("from_client");
    if (fd != NULL) {
        if (fd->flags == FLOW_PKT_TOSERVER && fd->match_cnt == 1) {
            result = 1;
        } else {
            printf("expected 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_TOSERVER, 1, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse08 is a test for setting the established,to_client flow opts
 */
int DetectFlowTestParse08 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("established,to_client");
    if (fd != NULL) {
        if (fd->flags & FLOW_PKT_ESTABLISHED && fd->flags & FLOW_PKT_TOCLIENT && fd->match_cnt == 2) {
            result = 1;
        } else {
            printf("expected: 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_ESTABLISHED + FLOW_PKT_TOCLIENT, 2, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse09 is a test for setting the to_client,stateless flow opts (order of state,dir reversed)
 */
int DetectFlowTestParse09 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("to_client,stateless");
    if (fd != NULL) {
        if (fd->flags & FLOW_PKT_STATELESS && fd->flags & FLOW_PKT_TOCLIENT && fd->match_cnt == 2) {
            result = 1;
        } else {
            printf("expected: 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_STATELESS + FLOW_PKT_TOCLIENT, 2, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse10 is a test for setting the from_server,stateless flow opts (order of state,dir reversed)
 */
int DetectFlowTestParse10 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("from_server,stateless");
    if (fd != NULL) {
        if (fd->flags & FLOW_PKT_STATELESS  && fd->flags & FLOW_PKT_TOCLIENT && fd->match_cnt == 2){
            result = 1;
        } else {
            printf("expected: 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_STATELESS + FLOW_PKT_TOCLIENT, 2, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse11 is a test for setting the from_server,stateless flow opts with spaces all around
 */
int DetectFlowTestParse11 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse(" from_server , stateless ");
    if (fd != NULL) {
        if (fd->flags & FLOW_PKT_STATELESS  && fd->flags & FLOW_PKT_TOCLIENT && fd->match_cnt == 2){
            result = 1;
        } else {
            printf("expected: 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_STATELESS + FLOW_PKT_TOCLIENT, 2, fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse12 is a test for setting an invalid seperator :
 */
int DetectFlowTestParse12 (void) {
    int result = 1;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("from_server:stateless");
    if (fd != NULL) {
        printf("expected: NULL got 0x%02X %d: ",fd->flags, fd->match_cnt);
        result = 0;
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse13 is a test for an invalid option
 */
int DetectFlowTestParse13 (void) {
    int result = 1;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("invalidoptiontest");
    if (fd != NULL) {
        printf("expected: NULL got 0x%02X %d: ",fd->flags, fd->match_cnt);
        result = 0;
        DetectFlowFree(fd);
    }

    return result;
}
/**
 * \test DetectFlowTestParse14 is a test for a empty option
 */
int DetectFlowTestParse14 (void) {
    int result = 1;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("");
    if (fd != NULL) {
        printf("expected: NULL got 0x%02X %d: ",fd->flags, fd->match_cnt);
        result = 0;
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse15 is a test for an invalid combo of options established,stateless
 */
int DetectFlowTestParse15 (void) {
    int result = 1;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("established,stateless");
    if (fd != NULL) {
        printf("expected: NULL got 0x%02X %d: ",fd->flags, fd->match_cnt);
        result = 0;
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse16 is a test for an invalid combo of options to_client,to_server
 */
int DetectFlowTestParse16 (void) {
    int result = 1;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("to_client,to_server");
    if (fd != NULL) {
        printf("expected: NULL got 0x%02X %d: ",fd->flags, fd->match_cnt);
        result = 0;
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse16 is a test for an invalid combo of options to_client,from_server
 * flowbit flags are the same
 */
int DetectFlowTestParse17 (void) {
    int result = 1;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("to_client,from_server");
    if (fd != NULL) {
        printf("expected: NULL got 0x%02X %d: ",fd->flags, fd->match_cnt);
        result = 0;
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse18 is a test for setting the from_server,stateless,stream_only flow opts (order of state,dir reversed)
 */
int DetectFlowTestParse18 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("from_server,established,stream_only");
    if (fd != NULL) {
        if (fd->flags & FLOW_PKT_ESTABLISHED && fd->flags & FLOW_PKT_TOCLIENT && fd->flags & FLOW_PKT_STREAMONLY && fd->match_cnt == 3) {
            result = 1;
        } else {
            printf("expected 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_ESTABLISHED + FLOW_PKT_TOCLIENT + FLOW_PKT_STREAMONLY, 3,
                    fd->flags, fd->match_cnt);
        }
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse19 is a test for one to many options passed to DetectFlowParse
 */
int DetectFlowTestParse19 (void) {
    int result = 1;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("from_server,established,stream_only,a");
    if (fd != NULL) {
        printf("expected: NULL got 0x%02X %d: ",fd->flags, fd->match_cnt);
        result = 0;
        DetectFlowFree(fd);
    }

    return result;
}
/**
 * \test DetectFlowTestParse20 is a test for setting from_server, established, no_stream
 */
int DetectFlowTestParse20 (void) {
    int result = 0;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("from_server,established,no_stream");
    if (fd != NULL) {
        if (fd->flags & FLOW_PKT_ESTABLISHED && fd->flags & FLOW_PKT_TOCLIENT && fd->flags & FLOW_PKT_NOSTREAM && fd->match_cnt == 3) {
            result = 1;
        } else {
            printf("expected 0x%02X cnt %d got 0x%02X cnt %d: ", FLOW_PKT_ESTABLISHED + FLOW_PKT_TOCLIENT + FLOW_PKT_NOSTREAM, 3,
                    fd->flags, fd->match_cnt);
        }

        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \test DetectFlowTestParse21 is a test for an invalid opt between to valid opts
 */
int DetectFlowTestParse21 (void) {
    int result = 1;
    DetectFlowData *fd = NULL;
    fd = DetectFlowParse("from_server,a,no_stream");
    if (fd != NULL) {
        printf("expected: NULL got 0x%02X %d: ",fd->flags, fd->match_cnt);
        result = 0;
        DetectFlowFree(fd);
    }

    return result;
}

/**
 * \brief this function registers unit tests for DetectFlow
 */
void DetectFlowRegisterTests(void) {
    UtRegisterTest("DetectFlowTestParse01", DetectFlowTestParse01, 1);
    UtRegisterTest("DetectFlowTestParse02", DetectFlowTestParse02, 1);
    UtRegisterTest("DetectFlowTestParse03", DetectFlowTestParse03, 1);
    UtRegisterTest("DetectFlowTestParse04", DetectFlowTestParse04, 1);
    UtRegisterTest("DetectFlowTestParse05", DetectFlowTestParse05, 1);
    UtRegisterTest("DetectFlowTestParse06", DetectFlowTestParse06, 1);
    UtRegisterTest("DetectFlowTestParse07", DetectFlowTestParse07, 1);
    UtRegisterTest("DetectFlowTestParse08", DetectFlowTestParse08, 1);
    UtRegisterTest("DetectFlowTestParse09", DetectFlowTestParse09, 1);
    UtRegisterTest("DetectFlowTestParse10", DetectFlowTestParse10, 1);
    UtRegisterTest("DetectFlowTestParse11", DetectFlowTestParse11, 1);
    UtRegisterTest("DetectFlowTestParse12", DetectFlowTestParse12, 1);
    UtRegisterTest("DetectFlowTestParse13", DetectFlowTestParse13, 1);
    UtRegisterTest("DetectFlowTestParse14", DetectFlowTestParse14, 1);
    UtRegisterTest("DetectFlowTestParse15", DetectFlowTestParse15, 1);
    UtRegisterTest("DetectFlowTestParse16", DetectFlowTestParse16, 1);
    UtRegisterTest("DetectFlowTestParse17", DetectFlowTestParse17, 1);
    UtRegisterTest("DetectFlowTestParse18", DetectFlowTestParse18, 1);
    UtRegisterTest("DetectFlowTestParse19", DetectFlowTestParse19, 1);
    UtRegisterTest("DetectFlowTestParse20", DetectFlowTestParse20, 1);
    UtRegisterTest("DetectFlowTestParse21", DetectFlowTestParse21, 1);
}
