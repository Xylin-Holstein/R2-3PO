#!/usr/bin/env python3
"""Guard the model-assisted associative retrieval path without requiring Ollama."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

helper_start = core.find("static char *r2_build_associative_search_query(")
helper_end = core.find("static char *chat_with_relevant_memories(", helper_start)
require(helper_start >= 0 and helper_end > helper_start,
        "an isolated associative query planner exists before normal retrieval")
helper = core[helper_start:helper_end]
require(
    "ollama_chat_with_limit(" in helper
    and "Do NOT answer the user" in helper
    and "natural paraphrases" in helper,
    "the local model interprets message meaning into paraphrase cues rather than relying on a recall trigger phrase",
)
require(
    "current-user identity" in helper
    and "linked Life Log records" in helper
    and "Do not assume any " in helper
    and "particular relationship" in helper,
    "identity questions retrieve generic relationship evidence without hardcoding who the user is",
)

keyword_start = core.index("static size_t extract_memory_keywords(")
keyword_end = core.index("static char *r2_build_associative_search_query(", keyword_start)
keyword_block = core[keyword_start:keyword_end]
require(
    '{"mother", "mom"}' not in keyword_block
    and '{"mother", "mommy"}' not in keyword_block
    and '{"mother", "mama"}' not in keyword_block
    and '{"father", "dad"}' not in keyword_block
    and '{"parent", "mother"}' not in keyword_block
    and '{"parent", "father"}' not in keyword_block
    and '{"remember", "recall"}' in keyword_block,
    "relationship identity is not inferred through hard-coded keyword aliases; generic recall retrieval remains available",
)

require(
    "original user text remains unchanged" in helper
    and "r2_reply_sanitize(generated)" in helper,
    "generated retrieval cues are treated as search input only and sanitized",
)
builder_start = core.find("static char *chat_with_relevant_memories(")
builder_end = core.find("/* ============================================================\n   STRING HELPERS", builder_start)
builder = core[builder_start:builder_end]
require(
    "r2_build_associative_search_query(query ? query : \"\")" in builder
    and "r2_associative_query_cache_lookup(query)" in helper
    and "r2_associative_query_cache_store(query, combined)" in helper,
    "semantic retrieval cues are reused across retries instead of repeating planner inference",
)
impl_start = core.find("static char *r2_talk_impl(const char *message)")
impl_end = core.find("char *r2_talk(const char *message)", impl_start)
turn_impl = core[impl_start:impl_end]
require(
    "r2_associative_query_cache_clear();" in turn_impl
    and turn_impl.find("r2_associative_query_cache_clear();")
        < turn_impl.find("chat_with_relevant_memories(message)"),
    "the associative plan cache is reset at the start of each turn, while live context is still rebuilt on retries",
)
require(
    "get_relevant_memories(associative_query, memory_limit)" in builder
    and "extract_memory_keywords(associative_query, keywords)" in builder,
    "generated associations are used to search persistent memory and Life Log history",
)

rank_start = core.find("static int r2_memory_has_keyword(")
rank_end = core.find("static char *get_relevant_memories(", rank_start)
require(
    rank_start >= 0 and rank_end > rank_start
    and "isalnum((unsigned char)match[-1])" in core[rank_start:rank_end]
    and "isalnum((unsigned char)match[length])" in core[rank_start:rank_end],
    "memory cues use whole-token boundaries instead of substring collisions",
)
memory_start = core.find("static char *get_relevant_memories(")
memory_end = core.find("\nstatic ", memory_start + 1)
memory_ranker = core[memory_start:memory_end]
require(
    "keyword_length >= 9 ? 4" in memory_ranker
    and "keyword_length >= 6 ? 2 : 1" in memory_ranker
    and "matched_keywords - 1" in memory_ranker,
    "specific concepts and multi-cue coverage receive more weight than short generic overlaps",
)
require(
    "char *memory_copy = xstrdup(memory);" in memory_ranker
    and "char *category_copy = xstrdup(category);" in memory_ranker
    and "char *updated_copy = xstrdup(updated);" in memory_ranker
    and "if (!memory_copy || !category_copy || !updated_copy)" in memory_ranker
    and "free(memory_copy);" in memory_ranker
    and "free(category_copy);" in memory_ranker
    and "free(updated_copy);" in memory_ranker
    and "candidates[insert_at].updated_at = updated_copy;" in memory_ranker,
    "allocation failure cannot put a partially initialized record into ranking or result formatting",
)
ranked_scan = memory_ranker.split("\n    } else {", 1)[0]
require(
    "calloc((size_t)limit, sizeof(*candidates))" in ranked_scan
    and "if (insert_at >= (size_t)limit)" in ranked_scan
    and "candidates[insert_at].score = score;" in ranked_scan
    and "candidates[j].score >" not in ranked_scan
    and '"ORDER BY updated_at DESC"' not in ranked_scan,
    "keyword ranking retains only top results and avoids quadratic C and full-table SQL sorts",
)
require(
    "r2_diary_search" in builder and "r2_visual_search" in builder,
    "the same associations also search the separate diary and visual experience stores",
)
association_start = core.find("static char *r2_collect_associated_records(")
association_end = core.find("static void r2_append_association_section(", association_start)
association_search = core[association_start:association_end] if association_start >= 0 and association_end > association_start else ""
require(
    "searches < 6" in association_search
    and "r2_diary_search, R2_MAX_LIVE_MESSAGE_BYTES" in builder
    and "r2_visual_search, R2_MAX_LIVE_MESSAGE_BYTES" in builder,
    "diary and visual retrieval use the shared live-message-sized evidence budget and multiple search slots",
)
require(
    "RELATED DIARY REFLECTIONS" in builder and "RELATED VISUAL EXPERIENCES" in builder,
    "diary and visual evidence is labeled as historical context rather than current fact",
)
require(
    "message_add(\"user\", message)" in core
    and "CURRENT TURN RETRIEVED CONTEXT" in builder,
    "the original user message remains a separate authoritative turn",
)
require(
    "int memory_limit = MAX_RELEVANT_MEMORIES;" in builder
    and "int history_search_limit = MAX_MEMORY_KEYWORDS;" in builder
    and "size_t history_char_limit = SIZE_MAX;" in builder,
    "ordinary turns use the full configured memory and keyword sets without a separate history-character quota",
)
require(
    "OLLAMA_MAX_SYSTEM_TOTAL_CHARS" not in core
    and "OLLAMA_MAX_TOTAL_CHARS" not in core
    and "OLLAMA_MAX_RECENT_MESSAGES" not in core
    and "r2_context_for_required_chars(required_chars, num_ctx)" in core
    and "if (required_chars > input_budget)" in core,
    "adaptive context allocation carries expanded retrieval into the actual model payload without fixed global caps",
)
print("R2 associative retrieval smoke test passed.")
