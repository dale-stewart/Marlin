Feature: Preparing the printer for a job

  Before a print starts, the slicer's opening lines tell the printer how hot to get,
  how fast to move, and how much filament to push. The printer must act on each of
  them and be able to say what it is set to, because that is what the host shows the
  person standing at the machine.

  Background:
    Given the printer is connected and idle

  Scenario: Setting the hotend temperature
    When the host sends "M104 S200"
    Then the hotend is aiming for 200 degrees

  Scenario: Waiting until the hotend is hot enough
    Given the hotend is already at 205 degrees
    When the host sends "M109 S200"
    Then the printer carries on without waiting

  Scenario: Cooling down does not hold up the job
    Given the hotend is already at 220 degrees
    When the host sends "M109 S180"
    Then the printer carries on without waiting

  Scenario: Setting the print speed
    When the host sends "M220 S150"
    Then the printer is running at 150% speed
    And asking the printer reports 150% speed

  Scenario: Setting how much filament to push
    When the host sends "M221 S80"
    Then asking the printer reports 80% flow

  Scenario: A dry run heats nothing
    Given the printer is in dry run mode
    When the host sends "M104 S250"
    Then the hotend is aiming for 0 degrees

  Scenario: Turning the fan on and off
    When the host sends "M106 S128"
    Then the fan is running at 128
    When the host sends "M107"
    Then the fan is stopped
