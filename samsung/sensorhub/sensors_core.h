/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SENSORS_CORE_H_
#define _SENSORS_CORE_H_

struct device;
struct device_attribute;
struct input_dev;

int sensors_create_symlink(struct input_dev *inputdev);
void sensors_remove_symlink(struct input_dev *inputdev);
int sensors_register(struct device *dev, void *drvdata,
	struct device_attribute *attributes[], char *name);
void sensors_unregister(struct device *dev,
	struct device_attribute *attributes[]);
void destroy_sensor_class(void);

#endif /* _SENSORS_CORE_H_ */
