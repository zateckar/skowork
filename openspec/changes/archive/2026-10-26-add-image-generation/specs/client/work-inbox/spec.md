# Spec Delta

## ADDED Requirements

### Requirement: Images In The Thread Feed
The Thread feed SHALL show image attachments that a tool stored, such as images from image.generate, inline at the point of the tool call, scaled to the feed width. The user can open an image at full size and save it to a location they choose.

#### Scenario: Generated image appears
- **WHEN** image.generate stores an image during a Run
- **THEN** the feed shows the image under that tool call's card without the user expanding the card

#### Scenario: Save an image
- **WHEN** the user chooses Save as on an image in the feed
- **THEN** the client asks for a location and writes the image there
