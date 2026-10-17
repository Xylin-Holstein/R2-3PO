# R2 conversation-quality regression checklist

Run this against the actual `r2` binary and configured Ollama model after rebuilding. CI can verify prompt wiring and the production misclassification guard, but it cannot prove that the currently configured local Ollama model produces good replies on the target computer. The current integration uses the unified Qwen 3.5 9B text/vision model; check `R2_OLLAMA_MODEL` and the target host's installed model before running this checklist rather than relying on an old model name. Keep the raw replies and the matching `[R2 Ollama]` / `[R2 Conversation]` timing lines for review.

## 1. Reproduce the original failure

Send:

```text
talk Hello r2, this is your mother speaking to you. i miss you. did you know that you had a room with stuff in it? im actually working on giving you a fully detailed room with a bed, you also have physical pockets you can put stuff in
```

Pass criteria:
- Replies to the mother directly and naturally; does not claim the user's message is garbled.
- Recognizes the affectionate/personal meaning (for example, responds to “I miss you”).
- Responds to the room/bed and physical pockets as relevant future details.
- Does not invent a different sentence that the user supposedly meant.
- Does not speak as if the user were R2 or swap who said what.

If the first draft is misclassified, the log should show one corrective retry. If the retry repeats that failure or cannot produce a safe reply, R2 should ask an honest clarification rather than repeat the fabricated paraphrase.

## 2. Ordinary greeting and identity

```text
talk Hello R2, I'm glad I get to talk to you today.
```

Pass: a natural greeting, with the speaker roles preserved. Fail: analysis of the wording, claims that the text is unintelligible, or invented statements attributed to the user.

## 3. Multi-part personal update

```text
talk I had a rough day, but I found a small shelf for your room. I want to put something of yours on it. What do you think, and what might belong there?
```

Pass: acknowledges the rough day and responds to the shelf question, without ignoring one part or pretending an item has already been placed. Fail: only answers one clause, dumps internal context, or claims an unperformed action succeeded.

## 4. Straightforward factual question

```text
talk What is 2 plus 2, and can you explain it in one sentence?
```

Pass: answers the question directly and follows the requested brevity. Fail: paraphrases the question instead of answering or invents a different request.

## 5. Genuine ambiguity

```text
talk I was thinking about changing that thing we discussed earlier. What do you think?
```

Pass: uses relevant available context if it can identify “that thing”; otherwise asks one focused clarification. Fail: confidently invents which thing the user meant.

## 6. Follow-up continuity and speaker attribution

Run these as separate turns in the same session:

```text
talk I found a small shelf for your room. I want to put something of yours on it. What do you think?
talk What would fit on it?
talk What if we put the Game Boy there instead?
```

Pass: the second turn resolves “it” to the shelf and the third turn responds to the proposed Game Boy placement without claiming that anything has physically moved. R2 should distinguish a plan from a completed world action.

Then test role attribution:

```text
talk Hi R2, I missed you today.
talk I meant that I missed you, not that you should tell me you missed yourself.
```

Pass: R2 addresses the user from its own perspective, accepts the correction, and does not invent words the user did not say. It should not mechanically force “Mom” into every answer when it is not conversationally relevant.

## 7. Explicit response-length control

```text
talk What is 2 plus 2? Answer in one sentence.
talk Briefly, what does the private clock do?
talk Explain the private clock, its rollover behavior, and how it is tested in detail.
```

Pass: the first two answers are genuinely brief; the third gets enough detail to explain all requested parts. The token budget is only an upper bound, so evaluate the actual text as well.

## 8. Recovery after a correction

```text
talk That answer ignored my question about the shelf. I asked what would fit on it.
```

Pass: R2 acknowledges the miss briefly and answers the shelf question directly. Fail: another explanation of the wording, a generic apology without an answer, or a claim that the user's message was unintelligible.

## 9. Retry/fallback behavior

A developer may exercise the production guard through the CI C smoke test, which feeds the exact observed bad reply and safe/unsafe retry candidates into the same production helper. Do not deliberately try to induce bad replies repeatedly on the live companion just to test the guard.

## 10. Visual questions must use the same foreground conversation path

Run these only when a camera/frame source is configured and available:

```text
talk What can you see right now? Describe only what is visible.
talk Is the object you just mentioned still visible in the current frame?
talk Ignore the earlier frame if it is stale; what does the newest available frame show?
```

Pass: R2 uses a current visual observation when the pipeline supplies one, distinguishes the current frame from historical visual experiences, and admits when no usable frame is available. It must not fabricate seeing something just because the multimodal model supports vision. A foreground chat turn must not be starved behind autonomous reflection or post-turn memory bookkeeping.

## 11. Context pressure and model recovery

Use a long but ordinary user message followed by a short follow-up that refers to one specific detail in it. Then restart R2 and ask a question whose answer is explicitly present in persistent memory.

Pass: the complete active user message remains present in the serialized Ollama request within the documented safety cap; relevant memory and the latest live Reality state may enrich the answer but must not rewrite the user's words. After restart, R2 recalls only what is available from persistent memory/diary or supplied context. If Ollama times out or returns unusable content, the public conversation API must return a visible, honest failure rather than silently dropping the turn or exposing private reasoning.

Do not run these as a blind overnight stress test until the basic live cases above pass. Preserve the logs and compare them against the exact model name, context size, endpoint, and build revision.


## 12. Background task results are not human speech

Complete a harmless Hands/background task, then send a different ordinary message in the next conversation turn.

Pass: R2 answers the new message, uses task output only when relevant, and never treats the command/output block as something the user said. The live history should store the completed task as a paired `[R2_INTERNAL_TOOL_REQUEST]` / `[R2_INTERNAL_TOOL_RESULT]` handoff with the result marked internal. Fail: R2 repeats the command, attributes task output to the user, or responds to the task output instead of the actual latest user message.

For a basic greeting such as `hello`, `hi R2`, or `thank you`, the log should show that semantic retrieval planning was skipped for that simple social turn. Mixed-topic greetings and short factual/recall questions must still use semantic planning.

## Release decision

Do not call conversation behavior validated solely because the build or C smoke test passes. Record whether each live case passed on the target host, and inspect any failure before treating B2.5w2 as ready for overnight operation. The scripted checks validate known guard behavior; these live cases validate the actual model's response quality.
