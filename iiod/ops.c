/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2014 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * */

#include "ops.h"
#include "parser.h"
#include "../debug.h"
#include "../iio.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>

int yyparse(yyscan_t scanner);

/* Corresponds to a thread reading from a device */
struct ThdEntry {
	SLIST_ENTRY(ThdEntry) next;
	pthread_cond_t cond;
	unsigned int nb;
	ssize_t err;
	FILE *fd;
	bool verbose;
};

/* Corresponds to an opened device */
struct DevEntry {
	SLIST_ENTRY(DevEntry) next;

	struct iio_device *dev;
	unsigned int sample_size;

	/* Linked list of ThdEntry structures corresponding
	 * to all the threads trying to read data */
	SLIST_HEAD(ThdHead, ThdEntry) thdlist_head;
	pthread_mutex_t thdlist_lock;

	pthread_t thd;
	pthread_attr_t attr;
};

/* This is a linked list of DevEntry structures corresponding to
 * all the devices which have threads trying to read them */
static SLIST_HEAD(DevHead, DevEntry) devlist_head =
	    SLIST_HEAD_INITIALIZER(DevHead);
static pthread_mutex_t devlist_lock = PTHREAD_MUTEX_INITIALIZER;

static ssize_t write_all(const void *src, size_t len, FILE *out)
{
	const void *ptr = src;
	while (len) {
		ssize_t ret = fwrite(ptr, 1, len, out);
		if (ret == 0)
			return -EIO;
		ptr += ret;
		len -= ret;
	}
	return ptr - src;
}

static void * read_thd(void *d)
{
	struct DevEntry *entry = d;
	struct ThdEntry *thd;
	unsigned int sample_size = entry->sample_size;
	ssize_t ret = 0;

	/* No more than 1024 bytes per read (arbitrary) */
	unsigned int max_size = 1024 / sample_size;

	while (true) {
		char *buf;
		unsigned long len, nb_samples = max_size;

		pthread_mutex_lock(&devlist_lock);

		/* We do this check here, because this while loop must exit
		 * with devlist_lock locked and thdlist_lock unlocked. */
		if (ret < 0)
			break;

		pthread_mutex_lock(&entry->thdlist_lock);
		if (SLIST_EMPTY(&entry->thdlist_head)) {
			pthread_mutex_unlock(&entry->thdlist_lock);
			break;
		}

		SLIST_FOREACH(thd, &entry->thdlist_head, next) {
			if (thd->nb < nb_samples)
				nb_samples = thd->nb;
		}

		pthread_mutex_unlock(&entry->thdlist_lock);

		len = nb_samples * sample_size;
		buf = malloc(len);
		if (!buf) {
			ret = -ENOMEM;
			break;
		}

		pthread_mutex_unlock(&devlist_lock);

		DEBUG("Reading %lu bytes from device\n", len);
		ret = iio_device_read_raw(entry->dev, buf, len);
		pthread_mutex_lock(&entry->thdlist_lock);
		nb_samples = ret / sample_size;

		SLIST_FOREACH(thd, &entry->thdlist_head, next) {
			size_t ret2;

			if (!thd->verbose) {
				fprintf(thd->fd, "%li\n", (long) ret);
			} else if (ret < 0) {
				char err_buf[1024];
				strerror_r(ret, err_buf, sizeof(err_buf));
				fprintf(thd->fd, "ERROR reading device: %s\n",
						err_buf);
			}
			if (ret < 0)
				continue;

			/* (nb_samples > thd->nb) may happen when
			 * the thread just connected. In this case we'll feed
			 * it with data on the next iteration. */
			if (nb_samples > thd->nb)
				continue;

			ret2 = write_all(buf, ret, thd->fd);
			if (ret2 > 0)
				thd->nb -= ret2 / sample_size;

			if (ret2 < 0) {
				SLIST_REMOVE(&entry->thdlist_head, thd,
						ThdEntry, next);
				thd->err = ret2;
				pthread_cond_signal(&thd->cond);
			} else if (thd->nb == 0) {
				SLIST_REMOVE(&entry->thdlist_head, thd,
						ThdEntry, next);
				thd->err = 0;
				pthread_cond_signal(&thd->cond);
			}
		}

		pthread_mutex_unlock(&entry->thdlist_lock);
		free(buf);
	}

	/* Signal all remaining threads */
	pthread_mutex_lock(&entry->thdlist_lock);
	SLIST_FOREACH(thd, &entry->thdlist_head, next) {
		SLIST_REMOVE(&entry->thdlist_head, thd,
				ThdEntry, next);
		if (ret < 0)
			thd->err = ret;
		pthread_cond_signal(&thd->cond);
	}
	pthread_mutex_unlock(&entry->thdlist_lock);

	DEBUG("Removing device %s from list\n",
			iio_device_get_id(entry->dev));
	SLIST_REMOVE(&devlist_head, entry, DevEntry, next);
	pthread_mutex_unlock(&devlist_lock);

	iio_device_close(entry->dev);
	pthread_mutex_destroy(&entry->thdlist_lock);
	pthread_attr_destroy(&entry->attr);
	free(entry);

	DEBUG("Thread terminated\n");
	return NULL;
}

static ssize_t read_buffer(struct parser_pdata *pdata, struct iio_device *dev,
		unsigned int nb, unsigned int sample_size)
{
	struct DevEntry *e, *entry = NULL;
	struct ThdEntry *thd;
	pthread_mutex_t mutex;
	ssize_t ret;

	pthread_mutex_lock(&devlist_lock);
	SLIST_FOREACH(e, &devlist_head, next) {
		if (e->dev == dev) {
			entry = e;
			break;
		}
	}

	/* Ensure that two threads reading the same device
	 * use the same sample size */
	if (entry && (entry->sample_size != sample_size)) {
		pthread_mutex_unlock(&devlist_lock);
		return -EINVAL;
	}

	thd = malloc(sizeof(*thd));
	if (!thd) {
		pthread_mutex_unlock(&devlist_lock);
		return -ENOMEM;
	}

	/* !entry: no DevEntry in the linked list corresponding to the
	 *         device, so we add one */
	if (!entry) {
		int ret;

		DEBUG("Creating entry\n");
		entry = malloc(sizeof(*entry));
		if (!entry) {
			free(thd);
			pthread_mutex_unlock(&devlist_lock);
			return -ENOMEM;
		}

		ret = iio_device_open(dev);
		if (ret) {
			free(entry);
			free(thd);
			pthread_mutex_unlock(&devlist_lock);
			return ret;
		}

		entry->dev = dev;
		entry->sample_size = sample_size;
		SLIST_INIT(&entry->thdlist_head);
		pthread_attr_init(&entry->attr);
		pthread_attr_setdetachstate(&entry->attr,
				PTHREAD_CREATE_DETACHED);

		ret = pthread_create(&entry->thd, &entry->attr,
				read_thd, entry);
		if (ret) {
			iio_device_close(dev);
			free(entry);
			free(thd);
			pthread_mutex_unlock(&devlist_lock);
			return ret;
		}

		SLIST_INSERT_HEAD(&devlist_head, entry, next);
	}

	thd->nb = nb;
	thd->fd = pdata->out;
	thd->verbose = pdata->verbose;
	pthread_cond_init(&thd->cond, NULL);

	DEBUG("Added thread to client list\n");
	pthread_mutex_lock(&entry->thdlist_lock);
	SLIST_INSERT_HEAD(&entry->thdlist_head, thd, next);
	pthread_mutex_unlock(&entry->thdlist_lock);
	pthread_mutex_unlock(&devlist_lock);

	pthread_mutex_init(&mutex, NULL);
	pthread_mutex_lock(&mutex);

	DEBUG("Waiting for completion...\n");
	pthread_cond_wait(&thd->cond, &mutex);

	fflush(thd->fd);

	ret = thd->err;
	free(thd);

	if (ret < 0)
		return ret;
	else
		return nb * sample_size;
}

static struct iio_device * get_device(struct iio_context *ctx, const char *id)
{
	unsigned int i, nb_devices = iio_context_get_devices_count(ctx);

	for (i = 0; i < nb_devices; i++) {
		struct iio_device *dev = iio_context_get_device(ctx, i);
		if (!strcmp(id, iio_device_get_id(dev))
				|| !strcmp(id, iio_device_get_name(dev)))
			return dev;
	}

	return NULL;
}

ssize_t read_dev(struct parser_pdata *pdata, const char *id,
		unsigned int nb, unsigned int sample_size)
{
	struct iio_device *dev = get_device(pdata->ctx, id);
	if (!dev) {
		if (pdata->verbose) {
			char buf[1024];
			strerror_r(ENODEV, buf, sizeof(buf));
			fprintf(pdata->out, "ERROR: %s\n", buf);
		} else {
			fprintf(pdata->out, "%i\n", -ENODEV);
		}
		return -ENODEV;
	}

	return read_buffer(pdata, dev, nb, sample_size);
}

ssize_t read_dev_attr(struct parser_pdata *pdata,
		const char *id, const char *attr)
{
	FILE *out = pdata->out;
	struct iio_device *dev = get_device(pdata->ctx, id);
	char buf[1024], cr = '\n';
	ssize_t ret;

	if (!dev) {
		if (pdata->verbose) {
			strerror_r(ENODEV, buf, sizeof(buf));
			fprintf(out, "ERROR: %s\n", buf);
		} else {
			fprintf(out, "%i\n", -ENODEV);
		}
		return -ENODEV;
	}

	ret = iio_device_attr_read(dev, attr, buf, 1024);
	if (pdata->verbose && ret < 0) {
		strerror_r(-ret, buf, sizeof(buf));
		fprintf(out, "ERROR: %s\n", buf);
	} else {
		fprintf(out, "%li\n", (long) ret);
	}
	if (ret < 0)
		return ret;

	ret = write_all(buf, ret, out);
	write_all(&cr, 1, out);
	return ret;
}

ssize_t write_dev_attr(struct parser_pdata *pdata,
		const char *id, const char *attr, const char *value)
{
	FILE *out = pdata->out;
	struct iio_device *dev = get_device(pdata->ctx, id);
	if (!dev) {
		if (pdata->verbose) {
			char buf[1024];
			strerror_r(ENODEV, buf, sizeof(buf));
			fprintf(out, "ERROR: %s\n", buf);
		} else {
			fprintf(out, "%i\n", -ENODEV);
		}
		return -ENODEV;
	} else {
		ssize_t ret = iio_device_attr_write(dev, attr, value);
		if (pdata->verbose && ret < 0) {
			char buf[1024];
			strerror_r(-ret, buf, sizeof(buf));
			fprintf(out, "ERROR: %s\n", buf);
		} else {
			fprintf(out, "%li\n", (long) ret);
		}
		return ret;
	}
}

void interpreter(struct iio_context *ctx, FILE *in, FILE *out, bool verbose)
{
	yyscan_t scanner;
	struct parser_pdata pdata;

	pdata.ctx = ctx;
	pdata.stop = false;
	pdata.in = in;
	pdata.out = out;
	pdata.verbose = verbose;

	yylex_init_extra(&pdata, &scanner);
	yyset_out(out, scanner);
	yyset_in(in, scanner);

	do {
		if (verbose) {
			fprintf(out, "iio-daemon > ");
			fflush(out);
		}
		yyparse(scanner);
		if (pdata.stop)
			break;
	} while (!feof(in));

	yylex_destroy(scanner);
}