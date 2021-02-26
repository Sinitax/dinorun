#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include <ncurses.h>
#include <time.h>

#define ctrl(x)   ((x) & 0x1f)
#define nocase(x) ((x) | 0b100000)

#define A(x) (x | A_ALTCHARSET)
#define N(x) (x)

#define FOR_ADVANCE(pos, start) pos = start; pos; pos = pos->next
#define FOR_ADVANCE_P(pos, start) pos = start; *pos; pos = &((*pos)->next)

#define ARRSIZE(x) (sizeof(x)/sizeof((x)[0]))

#define FRAME_TIME (1 / 30.f)

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

enum { BIRD, TREE };

enum {
	PF_NONE = 0,
	PF_JUMP = 1,
	PF_DEAD = 2,
	PF_FALL = 4
};

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

struct state {
	pid_t master;

	int reset, redraw, quit;
	int score;

	int screenw, screenh;
	int ww, wh;

	struct player p;
};

static const int screenminw = 42;
static const int screenminh = 30;

static const float groundy = 2;

static const int spawn_spacingmin  = 40;
static const int spawn_spacingvar  = 4;

static const int bird_spawnmaxh = 6;
static const int bird_spawnminh = 1;

static const float playerjumpvel = 20;
static const int playeracc = -13;

static const struct model playerm_run = {
	.w = 2, .h = 3, .parts = {
		A(102), N('>'), // °>
		A(120), N(')'), // |)
		N('v'), N('v')  // vv
	}
};
static const struct model playerm_duck = {
	.w = 5, .h = 1, .parts = {
		N('v'), N('v'), N('='), A(102), N('>') // vv=°>
	}
};

static const struct model birdm = {
	.w = 4, .h = 1, .parts = {
		N('<'), A(102), N('v'), N('-') // <°v-
	}
};

static const struct model treem1 ={
	.w = 3, .h = 4, .parts = {
		N(' '), A(120), N(' '), //   |
		A(109), A(110), A(106), // |---|
		N(' '), A(117), N(' '), //  -|
		N(' '), A(116), N(' ')  //   |-
	}
};
static const struct model treem2 = {
	.w = 1, .h = 3, .parts = {
		A(116),                 //   |-
		A(117),                 //  -|
		A(120)                  //   |
	}
};
static const struct model treem3 = {
	.w = 1, .h = 3, .parts = {
		A(116),                 //   |-
		A(116),                 //   |-
		A(117)                  //  -|
	}
};
static const int treemcount = 3;
static const struct model *treems[] = {
	&treem1, &treem2, &treem3
};

static struct player player;

static WINDOW *gamewin, *scorewin;

static int popup = 0, popup_confirm = 0;

static struct state *game;

static const int gamespeed = 100;

static struct obstacle *obstacles = NULL;

void
stubhandle(int s)
{

}

void
resetgame(struct state *game)
{
	game->p.y = groundy;
	game->p.m = &playerm_run;
}

void
drawmodel(WINDOW *w, int sx, int sy, const struct model *m)
{
	int x, y, maxx, maxy;

	getmaxyx(w, maxy, maxx);

	mvwaddch(w, maxy - 1, maxx - 1, 'O');
	mvwaddch(w, 0, maxx - 1, 'O');

	for (y = 0; y < m->h; y++) {
		for (x = 0; x < m->w; x++) {
			mvwaddch(w, maxy - sy - (m->h - 1) + y,
					sx + x, m->parts[y * m->w + x]);
		}
	}
}

void
dispupdate()
{
	/* initialize UI */
	wclear(stdscr);
	box(stdscr, 0, 0);
	wrefresh(stdscr);

	scorewin = newwin(3, 12, 1, game->ww - 14);
	box(scorewin, 0, 0);
	wrefresh(scorewin);

	game->ww = game->screenw - 2;
	game->wh = 14;
	gamewin = newwin(game->wh, game->ww,
			(game->screenh - game->wh) / 2,
			(game->screenw - game->ww) / 2);
	wrefresh(gamewin);

	srand(time(NULL));

	game->p.x = game->ww / 4.f;

	while (!game->quit && !game->redraw) {
		static float travel = 0;
		static long dist = 0, lastspawn = 50;
		struct obstacle *o, **op, *tmp;
		float nvel;
		int i, x, y;

		werase(gamewin);

		/* update player */
		nvel = game->p.vely + FRAME_TIME * (game->p.y - groundy) * playeracc;
		if (nvel < 0 && game->p.vely >= 0)
			game->p.flags &= ~PF_JUMP; /* reset jump buffer at height of jump */
		game->p.vely = nvel;
		if (game->p.y == groundy && (game->p.flags & PF_JUMP)) {
			game->p.vely = playerjumpvel;
			game->p.flags &= ~PF_JUMP;
		}
		game->p.y = MAX(groundy, game->p.y + game->p.vely * FRAME_TIME);
		if (game->p.y == groundy) game->p.vely = 0;

		/* move obstacles and check collisions */
		travel += FRAME_TIME * gamespeed;
		if (travel > 1) {
			for (op = &obstacles; *op;) {
				o = *op;
				o->x -= travel;

				/* TODO: make sure player cant glitch through obstacles */
				if (o->x < game->p.x + game->p.m->w && o->x >= game->p.x
						&& o->y < game->p.y + game->p.m->h && o->y >= game->p.y) {
					// COLLISION
					game->p.flags |= PF_DEAD;
				}

				if (o->x + o->m->w < -1) { /* despawn */
					*op = (*op)->next;
					free(o);
				} else {
					op = &((*op)->next);
				}
			}
			dist += travel;
			travel = 0;
		}

		/* generate new obstacles */
		if (dist > lastspawn + spawn_spacingmin + rand() % spawn_spacingvar) {
			int type;

			if (!(o = malloc(sizeof(struct obstacle)))) {
				perror("malloc()");
				game->quit = 1;
				exit(1);
			}

			o->type = rand() % 2;
			if (o->type == BIRD) {
				o->m = &birdm;
				o->y = bird_spawnminh + rand() % (bird_spawnmaxh + 1 - bird_spawnminh);
			} else if (o->type == TREE) {
				o->m = treems[rand() % treemcount];
				o->y = groundy;
			}

			o->x = game->screenw + 2;

			tmp = obstacles;
			obstacles = o;
			if (tmp) o->next = tmp;
			else o->next = NULL;
			lastspawn = dist;
		}

		/* draw obstacles */
		for (FOR_ADVANCE(o, obstacles)) {
			drawmodel(gamewin, o->x, o->y, o->m);
		}

		/* draw player */
		drawmodel(gamewin, game->p.x, game->p.y, game->p.m);

		/* display score */

		/* refresh windows */
		wnoutrefresh(scorewin);
		wnoutrefresh(gamewin);
		doupdate();

		game->score += FRAME_TIME * 10;

		if (game->p.flags & PF_DEAD) {
			// const char *deathmsg = "You traveled %i! Play again? [Y/N]";
			// TODO:
			// - show end card
			// - wait for input
			// - restart / exit
			game->quit = 1;
			kill(game->master, SIGINT);
			break;
		}

		usleep(FRAME_TIME * 1000000);
	}

	// cleanup
	delwin(scorewin);
	delwin(gamewin);
}

int
main(int argc, char** argv)
{
	pid_t pid;

	/* init ncurses */
	initscr();
	raw();
	noecho();
	curs_set(0);
	keypad(stdscr, TRUE);

	game = mmap(NULL, sizeof(struct state), PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1 , 0);

	if (!game) {
		perror("mmap()");
		return EXIT_FAILURE;
	}

	if ((pid = fork()) == -1) {
		perror("fork()");
		return EXIT_FAILURE;
	}

	if (pid != 0) { /* input handler */
		int c, stat = 0;

		close(STDOUT_FILENO); /* only input */

		game->master = getpid();

		while (!game->quit) {
			c = getch();
			if (c == KEY_RESIZE) {
				game->redraw = 1;
			} else if (c == ctrl('c') || c == ctrl('d')) {
				game->quit = 1;
			} else if (c == '\n' || c == KEY_ENTER) {
				if (popup) {
					popup_confirm = 1;
					game->redraw = 1;
				}
			} else if (nocase(c) == 's' || c == KEY_DOWN) {
				game->p.flags |= PF_FALL;
			} else if (nocase(c) == 'w' || c == KEY_UP || c == ' ') {
				game->p.flags |= PF_JUMP;
			}
			usleep(100);
		}

		waitpid(pid, 0, stat);
		exit(0);
	} else { /* gfx handler */

		close(STDIN_FILENO); /* only output */

		resetgame(game);

		while (!game->quit) {
			getmaxyx(stdscr, game->screenh, game->screenw);
			if (game->screenw < screenminw || game->screenh < screenminw) {
				wclear(stdscr);
				printw("[X] Too few rows / columns to run properly.");
				wrefresh(stdscr);
				usleep(100);
				continue;
			}

			wrefresh(stdscr);
			dispupdate();
		}
	}

	/* deinit ncurses */
	endwin();
}
