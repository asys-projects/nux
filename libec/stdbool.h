/*
  EC - An embedded non standard C library

  SPDX-License-Identifier:	BSD-2-Clause
*/

#ifndef EC_STDBOOL_H
#define EC_STDBOOL_H

/*
 * C99's boolean type: a conversion to bool gives 0 or 1 according to
 * whether the value compares equal to 0.  (An int typedef truncated
 * wider values: a 64-bit mask test of a high bit was false.)
 */
#define bool  _Bool
#define false 0
#define true  1
#define __bool_true_false_are_defined 1

#endif /* EC_STDBOOL_H */
