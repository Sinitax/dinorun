#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

#include <ncurses.h>
#include <time.h>

#define CTRL(x)   ((x) & 0x1f)
#define NOCASE(x) ((x) | 0b100000)

#define A(x) (x | A_ALTCHARSET)
#define N(x) (x)
#define I(x) (x | A_ITALIC)

#define ARRSIZE(x) (sizeof(x)/sizeof((x)[0]))

#define FRAME_RATE 30
#define FRAME_TIME (1 / (1.f * FRAME_RATE))

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

enum { BIRD, TREE };

enum { YES = 1, NO = 2 };

enum { JUMP = 1, DEAD = 2, DUCK = 4 };

struct obstacle {
	float x, y;
	int type;
	const struct model *m;

	struct obstacle *next;
};

struct model {
	int w, h;
	int parts[];
};

struct player {
	float x, y, vely;
	int flags;
	const struct model *m;
};

static const int screenminw = 60;
static const int screenminh = 30;

static const float groundy = 2;

static const int spawn_spacingmin = 50;
static const int spawn_spacingmax = 80;

static const int bird_spawnmaxh = 5;
static const int bird_spawnminh = 0;

static const float playerjumpvel = 35;
static const int playeracc = -18;

static const int gamespeed = 80;

static const struct model playerm_run1 = {
	.w = 2, .h = 3, .parts = {
		A(102), N('>'), // °>
		A(120), N(')'), // |)
		I('v'), N(' ')  // v
	}
};
static const struct model playerm_run2 = {
	.w = 2, .h = 3, .parts = {
		A(102), N('>'), // °>
		A(120), N(')'), // |)
		N(' '), I('v')  //  v
	}
};
static const struct model playerm_air = {
	.w = 2, .h = 3, .parts = {
		A(102), N('>'), // °>
		A(120), N(')'), // |)
		I('v'), I('v')  // vv
	}
};
static const struct model playerm_duck = {
	.w = 5, .h = 1, .parts = {
		I('v'), I('v'), N('='), A(102), N('>') // vv=°>
	}
};
static const struct model *playerm_runs[] = {
	&playerm_run1, &playerm_run2
};

static const struct model birdm = {
	.w = 4, .h = 1, .parts = {
		N('<'), A(102), N('v'), N('-') // <°v-
	}
};

static const struct model treem1 ={
	.w = 3, .h = 5, .parts = {
		N(' '), A(120), N(' '), //   |
		A(109), A(110), A(106), // |---|
		N(' '), A(117), N(' '), //  -|
		N(' '), A(116), N(' '), //   |-
		A(113), A(118), A(113)  //  -+-
	}
};
static const struct model treem2 = {
	.w = 1, .h = 4, .parts = {
		A(116),                 //   |-
		A(117),                 //  -|
		A(120),                 //   |
		A(118)                  //  -+-
	}
};
static const struct model treem3 = {
	.w = 1, .h = 3, .parts = {
		A(117),                 //  -|
		A(116),                 //   |-
		A(118)                  //  -+-
	}
};
static const int treemcount = 3;
static const struct model *treems[] = {
	&treem1, &treem2, &treem3
};

static WINDOW *gamewin, *scorewin;
static struct obstacle *obstacles = NULL;
static int floorlen = 0, *floor = NULL;
static struct player p;

static int reset, redraw, quit, confirm;

static long dist, lastspawn;

static int playerm_runi, groupi, groupmax, grouptype, nextspawn;
static float dxf = 0;
static int bird_lasty = 0;

static int lastduck, ticks;

static int screenw, screenh, ww, wh;

void
die(const char *errstr, ...)
{
	va_list ap;

	endwin();
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);

	exit(1);
}

void
handleresize()
{
	do {
		getmaxyx(stdscr, screenh, screenw);
		if (screenw < screenminw || screenh < screenminw) {
			clear();
			printw("[X] Too few rows / columns to run properly.");
			refresh();
			usleep(10000);
		}
	} while (screenw < screenminw || screenh < screenminw);

	ww = screenw - 2;
	wh = 14;
}

void
resetgame()
{
	struct obstacle *o, *tmp;

	memset(&p, 0, sizeof(struct player));
	p.y = groundy;
	p.m = &playerm_run1;

	dist = lastspawn = 0;
	groupi = groupmax = 0;
	nextspawn = 0;

	dxf = 0.f;

	lastspawn = 50;

	for (o = obstacles; o; o = tmp) {
		tmp = o->next;
		free(o);
	}
	obstacles = NULL;

	srand(time(NULL));
}

void
drawmodel(WINDOW *w, int sx, int sy, const struct model *m)
{
	int x, y, maxx, maxy;

	getmaxyx(w, maxy, maxx);

	for (y = 0; y < m->h; y++) {
		for (x = 0; x < m->w; x++) {
			mvwaddch(w, maxy - sy - (m->h - 1) + y,
					sx + x, m->parts[y * m->w + x]);
		}
	}
}

int
popup(const char *msg, ...)
{
	WINDOW *win;
	va_list ap;
	int i, c, len;

	va_start(ap, msg);
	len = vsnprintf(NULL, 0, msg, ap);
	va_end(ap);

	while (!quit && !confirm) {
		win = newwin(5, len + 4, (screenh - 5) / 2,
				(screenw - len - 4) / 2);
		wclear(win);
		box(win, 0, 0);

		wmove(win, 2, 2);
		va_start(ap, msg);
		vw_printw(win, msg, ap);
		va_end(ap);

		wnoutrefresh(stdscr);
		wnoutrefresh(win);
		doupdate();

		confirm = 0;

		while (!quit && !confirm) {
			c = getch();
			if (c == KEY_RESIZE) {
				wclear(stdscr);
				handleresize();
				break;
			} else if (c == '\n' || c == KEY_ENTER || NOCASE(c) == 'y') {
				confirm = YES;
			} else if (c == CTRL('c') || c == CTRL('d') || NOCASE(c) == 'n') {
				confirm = NO;
			}
			usleep(10000);
		}
	}

	delwin(win);

	return confirm;
}

void
dispupdate()
{
	/* initialize UI */
	wclear(stdscr);
	box(stdscr, 0, 0);
	wrefresh(stdscr);

	scorewin = newwin(3, 12, 1, screenw - 14);
	box(scorewin, 0, 0);
	wrefresh(scorewin);

	gamewin = newwin(wh, ww,
			(screenh - wh) / 2,
			(screenw - ww) / 2);
	wnoutrefresh(gamewin);

	p.x = ww / 4.f;

	while (!quit && !redraw && !reset) {
		struct obstacle *o, **op, *tmp;
		float nvel;
		int i, x, y, dx, c;

		/* handle input */
		while ((c = getch()) != ERR) {
			if (c == KEY_RESIZE) {
				redraw = 1;
			} else if (c == CTRL('c') || c == CTRL('d')) {
				quit = 1;
			} else if (NOCASE(c) == 's' || c == KEY_DOWN) {
				p.flags |= DUCK;
				lastduck = ticks;
			} else if (NOCASE(c) == 'w' || c == KEY_UP || c == ' ') {
				p.flags |= JUMP;
			}
		}

		werase(gamewin);

		/* update player */
		nvel = p.vely + FRAME_TIME * (p.y - groundy) * playeracc;
		if (nvel < 0 && p.vely >= 0)
			p.flags &= ~JUMP; /* reset jump buffer at height of jump */
		p.vely = nvel;

		if (p.y == groundy && (p.flags & JUMP)) {
			p.vely = playerjumpvel;
			p.flags &= ~JUMP & ~DUCK;
		} else if (p.y > groundy && (p.flags & DUCK)) {
			p.vely = -17;
			p.flags &= ~DUCK;
		}

		p.y = MAX(groundy, p.y + p.vely * FRAME_TIME);
		if (p.y == groundy) p.vely = 0;

		if ((p.flags & DUCK) && p.y == groundy) {
			p.m = &playerm_duck;
			if ((ticks - lastduck) > FRAME_RATE / 3.f)
				p.flags &= ~DUCK;
		} else if (p.y > groundy) {
			p.m = &playerm_air;
		} else {
			if ((ticks % 3) == 0)
				playerm_runi = !playerm_runi;
			p.m = playerm_runs[playerm_runi];
		}

		/* move obstacles and check collisions */
		dxf += FRAME_TIME * gamespeed;
		if (dxf >= 1.f) {
			int inx, iny;

			for (op = &obstacles; *op;) {
				o = *op;

				inx = (p.x + p.m->w > o->x - dxf
						&& p.x < o->x + o->m->w);
				iny = (p.y + p.m->h > o->y
						&& p.y < o->y + o->m->h);

				if (inx && iny) p.flags |= DEAD;

				o->x -= (int) dxf;
				if (o->x + o->m->w < -1) { /* despawn */
					*op = (*op)->next;
					free(o);
				} else {
					op = &((*op)->next);
				}
			}

			floorlen = MAX(0, floorlen - dxf);
			for (i = 0; i < floorlen; i++)
				floor[i] = floor[i + (int) dxf];

			dx = dxf;
			dxf -= (float) dx;
		}

		dist += dx;
		nextspawn = MAX(0, nextspawn - dx);

		for (i = floorlen; i < screenw; i++)
			floor[i] = (rand() % 9 == 0) ? A(118) : A(113);
		floorlen = screenw;

		/* generate new obstacles */
		if (groupi < groupmax || dist > lastspawn + spawn_spacingmin
				+ rand() % (spawn_spacingmax + 1 - spawn_spacingmin)) {
			if (!(o = malloc(sizeof(struct obstacle))))
				die("malloc: %s\n", strerror(errno));

			if (groupi >= groupmax) {
				grouptype = (rand() % 10 > 6) ? BIRD : TREE;
				if (grouptype == BIRD) {
					groupmax = 1 + rand() % 2;
				} else {
					groupmax = 1 + rand() % 4;
				}
				groupi = 0;
			}

			o->type = grouptype;
			if (o->type == BIRD) {
				o->m = &birdm;
				do {
					o->y = groundy + bird_spawnminh + rand() %
						(bird_spawnmaxh + 1 - bird_spawnminh);
				} while (groupi > 0 && o->y == bird_lasty);
				bird_lasty = o->y;
			} else if (o->type == TREE) {
				o->m = treems[rand() % treemcount];
				o->y = groundy - 1;
			}

			o->x = screenw + 2 + nextspawn;
			nextspawn += o->m->w + 1;

			tmp = obstacles;
			obstacles = o;
			if (tmp) o->next = tmp;
			else o->next = NULL;
			lastspawn = dist;

			groupi++;
		}

		/* draw floor */
		for (i = 0; i < ww - 2; i++)
			mvwaddch(gamewin, wh - groundy + 1, 1 + i, floor[i]);

		/* draw obstacles */
		for (o = obstacles; o; o = o->next)
			drawmodel(gamewin, o->x, o->y, o->m);

		/* draw player */
		drawmodel(gamewin, p.x, p.y, p.m);

		/* display score */
		mvwprintw(scorewin, 1, 2, "%8d", dist);

		/* refresh windows */
		wnoutrefresh(gamewin);
		wnoutrefresh(scorewin);
		doupdate();

		if (p.flags & DEAD) {
			if (popup("You traveled %i m! Play again? [Y/N]",
						dist) == NO) {
				quit = 1;
			} else {
				reset = 1;
				redraw = 1;
			}
			break;
		}

		ticks++;
		usleep(FRAME_TIME * 1000000);
	}

	// cleanup
	delwin(scorewin);
	delwin(gamewin);
}

int
main(int argc, char** argv)
{
	int i;

	/* init ncurses */
	initscr();
	raw();
	noecho();
	curs_set(0);
	keypad(stdscr, TRUE);
	nodelay(stdscr, TRUE);

	resetgame();

	while (!quit) {

		if (reset) resetgame();
		handleresize();

		if (!(floor = realloc(floor, screenw * sizeof(int))))
			die("realloc: %s\n", strerror(errno));
		floorlen = MIN(floorlen, screenw);

		redraw = 0;
		reset = 0;
		confirm = 0;

		dispupdate();
	}

	/* deinit ncurses */
	endwin();
}
