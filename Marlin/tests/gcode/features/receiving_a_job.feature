Feature: Receiving a job from a host

  A host sends a print one line at a time over a serial cable, and cables drop bytes.
  Each line can carry a number and a checksum so the printer can tell a line it has
  already run from one that arrived damaged, and refuse anything it cannot trust —
  because a line acted on in error moves a hot nozzle to the wrong place.

  Background:
    Given the printer is connected and idle

  Scenario: A plain command is carried out
    When the host sends "M220 S45"
    Then the printer is running at 45% speed

  Scenario: Lines are numbered so nothing is missed
    When the host sends line 1 of "M220 S11"
    And the host sends line 2 of "M220 S22"
    Then the printer is running at 22% speed
    And the printer is expecting line 3

  Scenario: A line that arrives out of order is refused
    Given the host has sent line 1 of "M220 S11"
    When the host sends line 9 of "M220 S99"
    Then the printer is still running at 11% speed

  Scenario: A line the printer has already run is not run twice
    Given the host has sent line 1 of "M220 S11"
    When the host sends line 1 of "M220 S11" again
    Then that command is not carried out a second time

  Scenario: A damaged line is refused
    When the host sends "M220 S88" with a damaged checksum
    Then the printer is not running at 88% speed

  Scenario: An intact line is carried out
    When the host sends "M220 S66" with a correct checksum
    Then the printer is running at 66% speed

  Scenario: The host can restart the numbering
    Given the host has sent line 1 of "M220 S11"
    When the host renumbers to line 500
    Then the printer is expecting line 501
    And a command sent as line 501 is carried out

  Scenario Outline: Lines that carry no command are ignored
    When the host sends "<line>"
    Then the printer carries out nothing

    Examples:
      |  line     |
      |           |
      | ; comment |
