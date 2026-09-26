	1 REM Draw a function on the screen. 
    2 REM The function is defined by the slope and the y-intercept.

    10 PLOT 0,87: DRAW 255,0
	20 PLOT 127,0: DRAW 0,175
	30 INPUT "Slope : ", s
    40 INPUT "y-intercept : ", e$
    50 LET previ = 1
    60 LET curri = 1
    70 INK curri
	80 LET t=0

	85 REM Draw the function
	90 FOR f=0 TO 255
	100 LET x=(f-128)*s/128: LET y=VAL e$
    110 IF y >= 0 THEN LET curri = 1
    120 IF y < 0 THEN LET curri = 2
    130 IF curri <> previ THEN INK curri: LET previ = curri
	140 IF ABS y>87 THEN LET t=0: GO TO 170
	150 IF NOT t THEN PLOT f,y+88: LET t=1: GO TO 170
	160 DRAW 1,y-old y
	170 LET old y=INT (y+.5)
	180 NEXT f
    190 INK 0