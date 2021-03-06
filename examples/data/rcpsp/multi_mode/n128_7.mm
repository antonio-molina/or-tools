************************************************************************
file with basedata            : cn128_.bas
initial value random generator: 890358655
************************************************************************
projects                      :  1
jobs (incl. supersource/sink ):  18
horizon                       :  136
RESOURCES
  - renewable                 :  2   R
  - nonrenewable              :  1   N
  - doubly constrained        :  0   D
************************************************************************
PROJECT INFORMATION:
pronr.  #jobs rel.date duedate tardcost  MPM-Time
    1     16      0       25        6       25
************************************************************************
PRECEDENCE RELATIONS:
jobnr.    #modes  #successors   successors
   1        1          3           2   3   4
   2        3          3           5   8   9
   3        3          2          11  13
   4        3          3           6  11  15
   5        3          3          10  12  14
   6        3          2           7   9
   7        3          1          16
   8        3          3          10  12  14
   9        3          2          14  17
  10        3          3          11  13  16
  11        3          1          17
  12        3          1          13
  13        3          2          15  17
  14        3          1          16
  15        3          1          18
  16        3          1          18
  17        3          1          18
  18        1          0        
************************************************************************
REQUESTS/DURATIONS:
jobnr. mode duration  R 1  R 2  N 1
------------------------------------------------------------------------
  1      1     0       0    0    0
  2      1     3       9    0    0
         2     3       0    3    0
         3     7       0    2    1
  3      1     5       0    5    4
         2     9      10    0    0
         3    10       3    0    0
  4      1     7       7    0    5
         2     8       0    3    5
         3    10       7    0    0
  5      1     5       7    0    0
         2     7       4    0    0
         3     7       0    4    0
  6      1     1       6    0    9
         2     6       0    4    0
         3     7       0    3    9
  7      1     6       0    8    8
         2     8       6    0    0
         3     9       0    7    0
  8      1     5       0    2    4
         2     5       0    5    0
         3     6       9    0    0
  9      1     2       8    0    0
         2     5       5    0    9
         3     9       0    6    0
 10      1     4       0    4   10
         2     5       9    0    0
         3    10       4    0    9
 11      1    10       0    4    0
         2    10       6    0    0
         3    10       5    0    7
 12      1     1       9    0    0
         2     2       5    0    7
         3     3       0    3    5
 13      1     6       5    0    0
         2     8       0    8    9
         3    10       0    6    6
 14      1     1       7    0    4
         2     8       0    4    4
         3    10       6    0    2
 15      1     3       6    0    6
         2     6       5    0    4
         3    10       4    0    0
 16      1     4       0    4    2
         2     9       0    3    0
         3     9       6    0    0
 17      1     3       0    2    0
         2     5       8    0    9
         3     9       7    0    7
 18      1     0       0    0    0
************************************************************************
RESOURCEAVAILABILITIES:
  R 1  R 2  N 1
   33   26   94
************************************************************************
