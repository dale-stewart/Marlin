Feature: Understanding a command line sent by the host

  A host sends the printer one line of G-code at a time. Before anything can move
  or heat, the firmware has to work out which command was asked for and what
  values came with it. Lines arrive from many different senders — slicers,
  terminals, SD files — so spacing, line numbers and checksums vary, and a
  malformed line must be refused rather than half-obeyed.

  Background:
    Given the printer is waiting for a command

  Scenario: A movement command with coordinates
    When the host sends "G0 X10 Z30"
    Then the printer understands the command "G0"
    And the value of "X" is 10
    And no value was given for "Y"

  Scenario: A command whose number has more than one digit
    When the host sends "M104 S200"
    Then the printer understands the command "M104"
    And the value of "S" is 200

  Scenario: A tool change command
    When the host sends "T1 S1"
    Then the printer understands the command "T1"
    And a value was given for "S"

  Scenario: A line numbered by the host
    When the host sends "N1234   M104 S200"
    Then the printer understands the command "M104"
    And the value of "S" is 200

  Scenario: A line protected by a checksum
    When the host sends "N1 G0 X10*85"
    Then the printer understands the command "G0"
    And the value of "X" is 10

  Scenario Outline: Spacing does not change the meaning of a line
    When the host sends "<line>"
    Then the printer understands the command "G0"
    And the value of "X" is 10

    Examples:
      | line           |
      | G0 X10         |
      | G0   X10       |
      | G0 X 10        |
      | G0X10          |
      |    G0 X10      |

  Scenario: Negative and fractional coordinates
    When the host sends "G0 X-10.5 Y20.25"
    Then the value of "X" is -10.5
    And the value of "Y" is 20.25

  Scenario: A message command takes the rest of the line
    When the host sends "M118 Hello World"
    Then the printer understands the command "M118"
    And the message is "Hello World"

  Scenario: A file selection command takes a path
    When the host sends "M32 !/path/to/file.g#"
    Then the printer understands the command "M32"
    And the message is "/path/to/file.g"

  Scenario Outline: A line that is not a command is refused
    When the host sends "<line>"
    Then the printer refuses the command

    Examples: no code number, unknown letter, or nothing at all
      | line    |
      | GX10    |
      | Q1 X10  |
      | NG0 X10 |
      |         |

  Scenario: A parameter given without a value
    When the host sends "G0 X"
    Then the printer understands the command "G0"
    And no value was given for "X"
    And the message is "X"

  Scenario: Only the first valueless parameter is taken as the message
    When the host sends "G0 X Y"
    Then the message is " Y"

  Scenario: A parameter that is not a letter is taken as the message
    When the host sends "M33 !/path/to/file.g#"
    Then the printer understands the command "M33"
    And the message is "!/path/to/file.g#"

  Scenario: A refused line does not leave the previous command in place
    Given the host has already sent "M118 Hello"
    When the host sends ""
    Then the printer refuses the command
    And there is no message
