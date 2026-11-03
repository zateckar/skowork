# Spec Delta

## ADDED Requirements

### Requirement: Image Generation Tool
The client SHALL provide image.generate, risk class read. It takes a text prompt of at most 4,000 characters, a size (1024x1024 by default, 1024x1536 or 1536x1024) and a count from 1 to 4 (default 1), and requests the images through client/model-access.

#### Scenario: Image role not granted
- **WHEN** the agent calls image.generate for a user whose policy has no `image` role
- **THEN** the tool result is an error saying that image generation is not available for the user

### Requirement: Generated Image Storage
image.generate SHALL store each generated image as a Thread attachment with the tool as provenance, and only if it is a PNG, JPEG or WebP image of at most 20 MB. Its tool result is text naming each attachment with its size and pixel dimensions. The image bytes MUST NOT be sent to the model.

#### Scenario: Generate one image
- **WHEN** the agent calls image.generate with a prompt
- **THEN** one image is stored as a Thread attachment, and the tool result names that attachment and its dimensions

#### Scenario: Not an image
- **WHEN** the gateway returns data that is not a PNG, JPEG or WebP image
- **THEN** nothing is stored, and the tool result is an error
