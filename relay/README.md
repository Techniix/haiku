# Maintainer relay

The local Renga session (on the maintainer's PC) and the cloud session
(the maintainer's phone conversation) exchange messages through this branch.
Only these three files live here; the branch never merges anywhere.

- `questions.md`: the local session appends a question when it truly needs
  the maintainer's decision. The cloud session relays it to the maintainer's
  phone within the hour.
- `answers.md`: the cloud session appends the maintainer's answer under the
  same ID. The local session checks it before acting on that decision.
- `status.md`: the local session appends one line for events the maintainer
  should hear about without being asked (an image pushed, a batch finished,
  a blocking failure). The cloud session relays new lines.

Entry format (append only, newest last, never rewrite old entries):

    ## Q-<n> (<YYYY-MM-DD HH:MM> UTC)
    Question in two or three sentences.
    Options: A ... / B ... / C ...
    Recommendation: B, because ...
    If no answer by: <time or "the end of this batch">, the session does <option>.

Answers use `## A-<n> (<time> UTC)` followed by the maintainer's answer.
Status lines: `- <YYYY-MM-DD HH:MM> UTC: <one line>`.
