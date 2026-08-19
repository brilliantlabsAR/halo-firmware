#ifndef BUTTON_H
#define BUTTON_H

#define LONG_PRESS_MS_MAX 4  /* Maximum number of long press levels */

enum button_action {
	BUTTON_LONG_PRESS,
	BUTTON_LONG_PRESS_LEVEL_1,
	BUTTON_LONG_PRESS_LEVEL_2,
	BUTTON_LONG_PRESS_LEVEL_3,
	BUTTON_SINGLE_CLICK,
	BUTTON_DOUBLE_CLICK,
	/* Fired once per press while STILL HELD, the moment the hold crosses
	 * the level-1 threshold. Levels 0/1 themselves fire on release (so a
	 * longer hold can still reach the higher immediate-fire levels); this
	 * event exists to give the user release-now feedback at the crossing. */
	BUTTON_LONG_PRESS_LEVEL_1_HELD,
	BUTTON_EVENT_NUM
};

typedef void (*button_event_cb_t)(const struct device *dev, enum button_action action);

/* set all button event */
int button_callback_register(const struct device *dev, button_event_cb_t cb);
/* set a button event */
int button_event_callback_register(const struct device *dev, button_event_cb_t cb, enum button_action action);

#endif