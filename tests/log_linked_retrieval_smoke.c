#include "Log.h"
#include "SensoryJournal.h"
#include "Addiction.h"
#include "r2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void r2_sj_index_event(int64_t event_id, const char *category,
                       const char *event_type, const char *source)
{
    (void)event_id; (void)category; (void)event_type; (void)source;
}

int r2_save_memory(const char *memory, const char *category)
{
    (void)memory; (void)category;
    return 0;
}

int r2_addiction_record_choice(const char *target, const char *target_type,
                               int enjoyment_0_100, const char *source,
                               const char *details)
{
    (void)target; (void)target_type; (void)enjoyment_0_100;
    (void)source; (void)details;
    return 0;
}

int main(void)
{
    if (r2_log_init() != 0) {
        fprintf(stderr, "Life Log initialization failed.\n");
        return 1;
    }

    int64_t other = r2_log_event(R2_LOG_MEMORY, "related_experience",
                                 "related prior experience",
                                 "context reachable through explicit relationship links",
                                 "link-smoke");
    int64_t linked = r2_log_event(R2_LOG_MEMORY, "identity_relation",
                                  "explicit current-user identity relationship",
                                  "subject=current_user; relation=mother_of; object=R2",
                                  "link-smoke");
    int64_t seed = r2_log_event(R2_LOG_MEMORY, "identity_seed",
                                "unique identity seed event",
                                "the user identity is linked from this event",
                                "link-smoke");
    if (seed <= 0 || linked <= 0 || other <= 0 ||
        r2_log_link(seed, linked, "identity_evidence",
                    "explicit statement stored as a durable relationship link") != 0 ||
        r2_log_link(linked, other, "relationship_context",
                    "another explicit edge from the matching relationship event") != 0) {
        fprintf(stderr, "Could not create linked identity test records.\n");
        r2_log_shutdown();
        return 1;
    }

    char *from_seed = r2_log_search("identity", 10);
    char *from_relation = r2_log_search("explicit current-user identity relationship", 10);
    int ok = from_seed && from_relation &&
             strstr(from_seed, "explicit current-user identity relationship") &&
             strstr(from_seed, "identity_evidence") &&
             strstr(from_seed, "subject=current_user; relation=mother_of; object=R2") &&
             strstr(from_seed, "related prior experience") &&
             strstr(from_seed, "relationship_context") &&
             strstr(from_relation, "unique identity seed event") &&
             strstr(from_relation, "identity_evidence") &&
             strstr(from_relation, "related prior experience");
    if (!ok) {
        fprintf(stderr, "Life Log search did not traverse explicit links in both directions.\n");
        if (from_seed) fprintf(stderr, "Seed search:\n%s\n", from_seed);
        if (from_relation) fprintf(stderr, "Relation search:\n%s\n", from_relation);
    }
    free(from_seed);
    free(from_relation);
    r2_log_shutdown();
    if (!ok) return 1;
    puts("Life Log linked retrieval traverses explicit relationships in both directions.");
    return 0;
}
