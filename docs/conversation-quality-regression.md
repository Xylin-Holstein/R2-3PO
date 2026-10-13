# R2 conversation-quality regression checklist

Run this against the actual `r2` binary and configured Ollama model after rebuilding. CI can verify prompt wiring and the production misclassification guard, but it cannot prove that Gemma produces good replies on the target computer. Keep the raw replies and the matching `[R2 Ollama]` / `[R2 Conversation]` timing lines for review.

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

## 6. Retry/fallback behavior

A developer may exercise the production guard through the CI C smoke test, which feeds the exact observed bad reply and safe/unsafe retry candidates into the same production helper. Do not deliberately try to induce bad replies repeatedly on the live companion just to test the guard.

## Release decision

Do not call conversation behavior validated solely because the build or C smoke test passes. Record whether each live case passed on the target host, and inspect any failure before treating B2.5w2 as ready for overnight operation. The scripted checks validate known guard behavior; these live cases validate the actual model's response quality.
