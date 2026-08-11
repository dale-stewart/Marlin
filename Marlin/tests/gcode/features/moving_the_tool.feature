Feature: Moving the tool where it was told

  Everything a printer does, it does by moving. A job is a few hundred thousand coordinates,
  and the only thing standing between them and a finished part is that the tool arrives where
  each one said and the machine agrees afterwards about where that was.

  Three things have to hold together for that. The tool has to reach the coordinate. The
  printer's idea of where it is has to match where it actually is, or every coordinate after
  the first is measured from the wrong place. And a coordinate the machine cannot reach has to
  be refused rather than attempted, because the frame is what stops the carriage otherwise.

  Background:
    Given the printer is homed and ready

  Scenario: Going to a coordinate
    When the host sends the tool to X50 Y30
    Then the tool is reported at X50 Y30
    And the carriage really is 50 mm along X

  Scenario: Going somewhere else afterwards
    Given the host sends the tool to X50 Y30
    When the host sends the tool to X20 Y60
    Then the tool is reported at X20 Y60
    And the carriage really is 20 mm along X

  Scenario: Relative moves add to where the tool already is
    Given the host sends the tool to X50 Y30
    And the host switches to relative moves
    When the host sends the tool 10 mm further along X
    And the host sends the tool 10 mm further along X
    Then the tool is reported at X70 Y30

  Scenario: Absolute moves go to the coordinate however the tool got there
    Given the host switches to relative moves
    And the host sends the tool 10 mm further along X
    When the host switches back to absolute moves
    And the host sends the tool to X50 Y30
    Then the tool is reported at X50 Y30

  Scenario: Telling the printer where it is without moving it
    Given the host sends the tool to X50 Y30
    When the host declares the current position to be X0
    Then the tool is reported at X0 Y30
    And the carriage has not moved

  Scenario: A coordinate off the end of the bed is not attempted
    When the host sends the tool a metre beyond the end of X
    Then the carriage stops at the end of its travel
