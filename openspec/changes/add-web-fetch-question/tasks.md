# Tasks

## 1. Tools

- [x] 1.1 Add `question` to the web.fetch definition and description (design D5), rejecting it with format raw before any request. Verify with a unit test: no network request is made for a raw question. (WebFetch.AQuestionWithRawFormatFailsBeforeAnyRequest.)
- [x] 1.2 Answer the question with one `small` request without tools, using the prompt of design D2 and the page inside the untrusted-data delimiter, cutting the page to fit (design D3), and build the result of design D4. Verify with unit tests: the result has the answer, title, URL and length; a page above the budget gets the cut note; the result keeps untrusted provenance. (WebFetch.AQuestionIsAnsweredBySmallModelInsteadOfThePageText, APageLargerThanTheSmallWindowIsCutAndTheResultSaysSo.)
- [x] 1.3 Fall back to the page text with a note when the `small` role is missing or its request fails (design D4). Verify with unit tests using an in-test fault and a model list without `small`. (WebFetch.WithoutASmallModelTheQuestionFallsBackToThePageText.)

## 2. Verification

- [ ] 2.1 Against gamma on the test machine, ask a question about a long public documentation page with and without `question`, and record in design.md the tokens used by the Run and whether the answers agree.
- [ ] 2.2 Run `openspec validate --all --strict`.
