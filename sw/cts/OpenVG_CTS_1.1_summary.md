# OpenVG Conformance Test Suite Summary

## Revision History

```
1.0 January 2007 - first revision of OpenVG Conformance Test Suite
1.1 December 2008 – updated for OpenVG CTS 1.
```
The conformance test suite consists of over 900 test cases, grouped into 14 groups of tests.
Conformant implementation must pass all test cases.
Details on testing methodology, rating functions and tolerance values of particular tests are
provided in the spreadsheet: “CTS 1.1 summary.xls”.

## 1 Group A

Group A performs testing of API coverage, validation of parameters and error reporting. All
tests in this group must pass in order to continue the testing.

## 2 Group B

Test cases from group B perform rendering quality testing. Results of test B10101 – B
are verified via pyramid diff method with tolerance = 3. The remaining tests require certain
values to be matched.

## 3 Group C

Group C performs matrix operations. All test cases, but C10802 produce numerical results
and these have to be exact in order to pass the test. Test C10802 verifies resulting image
via pyramid diff with tolerance = 11.

## 4 Group D

Group D tests vgClear. Result compared pixel by pixel.

## 5 Group E

Scissoring operations tested in this group produce images that must match reference
images on pixel by pixel basis within tolerance.
Exception: tests E10104 and 10302 are verified by pyramid diff with tolerance 9 and 7
respectively.

## 6 Group F

Masking.
Tests F10101 and F10201 perform image diff comparison.
Test F20101 requires exact match of compared values.
Remaining test cases are verified by pyramid diff.

## 7 Group G

Group G exercises path operations. Details as to the verification method as well as tolerance
values are provided in the spreadsheet.

OpenVG Conformance Test Suite Summary – rev 1.1 1


## 8 Group H

Group H tests image operations. See spreadsheet for details.

## 9 Group I

Test cases from group I are paint related.
Tests I10101, I10103 and I10201 – I20101 perform image diff requiring exact match.
Tests I10102 and I10104 perform value diff and require exact match.
Remaining tests from I group are tested against reference images by pyramid diff.

## 10 Group J

Image filtering.
Tests J10101 – 20204 and J30101 – J30201 perform image diff (tolerance=3).
J40101 performs value diff – exact match required.
Remaining tests from this group use pyramid diff.

## 11 Group K

Blending and Color Tranform.
K20201 – value diff – exact match required.
Remaining tests – pyramid diff.

## 12 Group L

Group L tests Glyph API. Test cases L10101 – L10302 concern on validation of parameters
and error handling.
Remaining test perform rendering of glyphs and results are verified by pyramid diff.

## 13 Group M

VGU testing. Test cases M10101 and M10102 – value diff – exact match required.
The rest of test cases use pyramid diff with tolerance = 3.

## 14 Group N

Group N tests against color bleeding. Tests are performed only for MSAA configs. Exact
match required.

OpenVG Conformance Test Suite Summary – rev 1.1 2


