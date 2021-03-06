************************************************************************
file with basedata            : cm415_.bas
initial value random generator: 1760397904
************************************************************************
projects                      :  1
jobs (incl. supersource/sink ):  18
horizon                       :  140
RESOURCES
  - renewable                 :  2   R
  - nonrenewable              :  2   N
  - doubly constrained        :  0   D
************************************************************************
PROJECT INFORMATION:
pronr.  #jobs rel.date duedate tardcost  MPM-Time
    1     16      0       27        7       27
************************************************************************
PRECEDENCE RELATIONS:
jobnr.    #modes  #successors   successors
   1        1          3           2   3   4
   2        4          3           5  10  12
   3        4          3           6   7   9
   4        4          3           8  10  12
   5        4          3           7  11  15
   6        4          1          10
   7        4          1          17
   8        4          3           9  11  13
   9        4          2          15  16
  10        4          1          13
  11        4          2          16  17
  12        4          2          14  17
  13        4          1          14
  14        4          2          15  16
  15        4          1          18
  16        4          1          18
  17        4          1          18
  18        1          0        
************************************************************************
REQUESTS/DURATIONS:
jobnr. mode duration  R 1  R 2  N 1  N 2
------------------------------------------------------------------------
  1      1     0       0    0    0    0
  2      1     1       7    9    0    6
         2     4       6    8    0    6
         3     7       5    3    0    6
         4     9       3    2    0    6
  3      1     1       6    9    0    7
         2     3       6    9    4    0
         3     6       6    9    0    5
         4    10       4    7    0    2
  4      1     4       9    6   10    0
         2     9       6    6    0    6
         3     9       7    5   10    0
         4    10       4    5    0    6
  5      1     2       6    8    3    0
         2     5       6    6    2    0
         3     6       5    6    1    0
         4     7       5    3    0    2
  6      1     7       6    9    0    6
         2     8       5    7    8    0
         3     9       3    4    0    4
         4    10       3    2    6    0
  7      1     5       7    6    4    0
         2     8       6    6    0    7
         3     8       5    6    0    9
         4    10       5    6    0    4
  8      1     1       7    8    5    0
         2     2       7    8    0    9
         3     3       6    8    0    6
         4     5       6    7    0    5
  9      1     5       9    7    0    5
         2     9       7    5    7    0
         3     9       8    7    0    4
         4    10       6    5    5    0
 10      1     5       9    7    2    0
         2     8       9    6    0    6
         3     8       9    4    1    0
         4     9       9    2    0    6
 11      1     5       7    5    3    0
         2     6       5    4    0    6
         3     7       3    4    0    5
         4     7       4    4    3    0
 12      1     3       6    5    0   10
         2     4       6    5    0    9
         3     5       5    4    6    0
         4     6       4    3    2    0
 13      1     7       5    6    7    0
         2     9       3    5    0    9
         3    10       1    5    6    0
         4    10       1    5    0    5
 14      1     2       6    7    5    0
         2     2       6    6    6    0
         3     5       6    6    4    0
         4    10       4    6    0    1
 15      1     5       8    6    8    0
         2     6       8    5    0    6
         3     6       8    5    8    0
         4     8       6    5    8    0
 16      1     1       7    7    0    2
         2     3       6    7    5    0
         3     8       6    4    5    0
         4    10       5    4    2    0
 17      1     1       3    9    6    0
         2     3       2    7    0    8
         3     4       2    5    4    0
         4     9       1    3    0    7
 18      1     0       0    0    0    0
************************************************************************
RESOURCEAVAILABILITIES:
  R 1  R 2  N 1  N 2
   29   26   42   52
************************************************************************
