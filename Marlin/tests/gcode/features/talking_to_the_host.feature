Feature: Keeping the host informed while it works through commands

  A host manages a print unattended, watching only what comes back over the wire. It
  needs to know a line was refused rather than silently swallowed — a line that vanishes
  looks identical to one that succeeded, and the difference is exactly the information a
  host needs to stop and warn the person at the machine. And when someone is debugging a
  connection, the printer can be asked to show its own working, one line at a time.

  Background:
    Given the printer is connected and idle

  Scenario Outline: A command the printer does not recognise is refused, not silently dropped
    When the host sends "<line>"
    Then the printer says it does not understand "<line>"

    Examples:
      | line   |
      | Q1     |
      | M973   |

  Scenario: Asked to show its working, the printer echoes back what it is told
    Given the host has asked the printer to echo back the commands it receives
    When the host sends "M220 S45"
    Then the printer echoes back "M220 S45"
