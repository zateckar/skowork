# Spec Delta

## ADDED Requirements

### Requirement: Web Fetch Question
web.fetch SHALL accept an optional `question` of at most 1,000 characters with format text. The client then sends the page's readable text, cut to fit the `small` role's context budget, and the question to the `small` model role through client/model-access without tools, and returns its answer with the page title, final URL and page length instead of the page text. A cut is stated in the result. The answer is untrusted content with the URL as provenance.

#### Scenario: Question about a long page
- **WHEN** the agent calls web.fetch with a question on a permitted page of 80 KB of readable text
- **THEN** the result contains the `small` role's answer, the page title, the final URL and the page length, and not the page text, and it is marked untrusted with the URL as provenance

#### Scenario: Page larger than the small role's budget
- **WHEN** the page text does not fit the `small` role's context budget
- **THEN** the text is cut to fit, and the result states how many kilobytes of the page the answer is based on

### Requirement: Web Fetch Question Fallback
When the user has no `small` role or its request fails, web.fetch SHALL return the page text as it would without a question, with a note that the question could not be answered. A question with format raw MUST fail before any request is made. A page answered with a status other than 2xx is returned as it would be without a question, with its status, and no question is asked of it.

#### Scenario: Small role not granted
- **WHEN** the agent calls web.fetch with a question for a user whose policy grants no `small` role
- **THEN** the result is the page text, preceded by a note that the question could not be answered

#### Scenario: Question with raw format
- **WHEN** the agent calls web.fetch with a question and format raw
- **THEN** the call fails with a validation error and no request is sent
