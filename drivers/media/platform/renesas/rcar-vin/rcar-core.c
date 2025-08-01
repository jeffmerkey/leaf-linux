// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Renesas R-Car VIN
 *
 * Copyright (C) 2025 Niklas Söderlund <niklas.soderlund@ragnatech.se>
 * Copyright (C) 2016 Renesas Electronics Corp.
 * Copyright (C) 2011-2013 Renesas Solutions Corp.
 * Copyright (C) 2013 Cogent Embedded, Inc., <source@cogentembedded.com>
 * Copyright (C) 2008 Magnus Damm
 */

#include <linux/idr.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <media/v4l2-async.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>

#include "rcar-vin.h"

/*
 * The companion CSI-2 receiver driver (rcar-csi2) is known
 * and we know it has one source pad (pad 0) and four sink
 * pads (pad 1-4). So to translate a pad on the remote
 * CSI-2 receiver to/from the VIN internal channel number simply
 * subtract/add one from the pad/channel number.
 */
#define rvin_group_csi_pad_to_channel(pad) ((pad) - 1)
#define rvin_group_csi_channel_to_pad(channel) ((channel) + 1)

/*
 * Not all VINs are created equal, master VINs control the
 * routing for other VIN's. We can figure out which VIN is
 * master by looking at a VINs id.
 */
#define rvin_group_id_to_master(vin) ((vin) < 4 ? 0 : 4)

#define v4l2_dev_to_vin(d)	container_of(d, struct rvin_dev, v4l2_dev)

/* -----------------------------------------------------------------------------
 * Gen3 Group Allocator
 */

/* FIXME:  This should if we find a system that supports more
 * than one group for the whole system be replaced with a linked
 * list of groups. And eventually all of this should be replaced
 * with a global device allocator API.
 *
 * But for now this works as on all supported systems there will
 * be only one group for all instances.
 */

static DEFINE_IDA(rvin_ida);
static DEFINE_MUTEX(rvin_group_lock);
static struct rvin_group *rvin_group_data;

static void rvin_group_cleanup(struct rvin_group *group)
{
	media_device_cleanup(&group->mdev);
	mutex_destroy(&group->lock);
}

static int rvin_group_init(struct rvin_group *group, struct rvin_dev *vin,
			   int (*link_setup)(struct rvin_group *),
			   const struct media_device_ops *ops)
{
	struct media_device *mdev = &group->mdev;
	const struct of_device_id *match;
	struct device_node *np;

	mutex_init(&group->lock);

	/* Count number of VINs in the system */
	group->count = 0;
	for_each_matching_node(np, vin->dev->driver->of_match_table)
		if (of_device_is_available(np))
			group->count++;

	vin_dbg(vin, "found %u enabled VIN's in DT", group->count);

	group->link_setup = link_setup;

	mdev->dev = vin->dev;
	mdev->ops = ops;

	match = of_match_node(vin->dev->driver->of_match_table,
			      vin->dev->of_node);

	strscpy(mdev->driver_name, KBUILD_MODNAME, sizeof(mdev->driver_name));
	strscpy(mdev->model, match->compatible, sizeof(mdev->model));

	media_device_init(mdev);

	return 0;
}

static void rvin_group_release(struct kref *kref)
{
	struct rvin_group *group =
		container_of(kref, struct rvin_group, refcount);

	mutex_lock(&rvin_group_lock);

	rvin_group_data = NULL;

	rvin_group_cleanup(group);

	kfree(group);

	mutex_unlock(&rvin_group_lock);
}

static int rvin_group_get(struct rvin_dev *vin,
			  int (*link_setup)(struct rvin_group *),
			  const struct media_device_ops *ops)
{
	struct rvin_group *group;
	int ret;

	/* Join or create a VIN group */
	mutex_lock(&rvin_group_lock);
	if (rvin_group_data) {
		group = rvin_group_data;
		kref_get(&group->refcount);
	} else {
		group = kzalloc(sizeof(*group), GFP_KERNEL);
		if (!group) {
			ret = -ENOMEM;
			goto err_group;
		}

		ret = rvin_group_init(group, vin, link_setup, ops);
		if (ret) {
			kfree(group);
			vin_err(vin, "Failed to initialize group\n");
			goto err_group;
		}

		kref_init(&group->refcount);
		group->info = vin->info;

		rvin_group_data = group;
	}
	mutex_unlock(&rvin_group_lock);

	/* Add VIN to group */
	mutex_lock(&group->lock);

	if (group->vin[vin->id]) {
		vin_err(vin, "Duplicate renesas,id property value %u\n", vin->id);
		mutex_unlock(&group->lock);
		kref_put(&group->refcount, rvin_group_release);
		return -EINVAL;
	}

	group->vin[vin->id] = vin;

	vin->group = group;
	vin->v4l2_dev.mdev = &group->mdev;

	mutex_unlock(&group->lock);

	return 0;
err_group:
	mutex_unlock(&rvin_group_lock);
	return ret;
}

static void rvin_group_put(struct rvin_dev *vin)
{
	struct rvin_group *group = vin->group;

	mutex_lock(&group->lock);

	vin->group = NULL;
	vin->v4l2_dev.mdev = NULL;

	if (WARN_ON(group->vin[vin->id] != vin))
		goto out;

	group->vin[vin->id] = NULL;
out:
	mutex_unlock(&group->lock);

	kref_put(&group->refcount, rvin_group_release);
}

/* group lock should be held when calling this function. */
static int rvin_group_entity_to_remote_id(struct rvin_group *group,
					  struct media_entity *entity)
{
	struct v4l2_subdev *sd;
	unsigned int i;

	sd = media_entity_to_v4l2_subdev(entity);

	for (i = 0; i < ARRAY_SIZE(group->remotes); i++)
		if (group->remotes[i].subdev == sd)
			return i;

	return -ENODEV;
}

static int rvin_group_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct rvin_dev *vin = v4l2_dev_to_vin(notifier->v4l2_dev);
	unsigned int i;
	int ret;

	ret = media_device_register(&vin->group->mdev);
	if (ret)
		return ret;

	ret = v4l2_device_register_subdev_nodes(&vin->v4l2_dev);
	if (ret) {
		vin_err(vin, "Failed to register subdev nodes\n");
		return ret;
	}

	/* Register all video nodes for the group. */
	for (i = 0; i < RCAR_VIN_NUM; i++) {
		if (vin->group->vin[i] &&
		    !video_is_registered(&vin->group->vin[i]->vdev)) {
			ret = rvin_v4l2_register(vin->group->vin[i]);
			if (ret)
				return ret;
		}
	}

	return vin->group->link_setup(vin->group);
}

static void rvin_group_notify_unbind(struct v4l2_async_notifier *notifier,
				     struct v4l2_subdev *subdev,
				     struct v4l2_async_connection *asc)
{
	struct rvin_dev *vin = v4l2_dev_to_vin(notifier->v4l2_dev);
	struct rvin_group *group = vin->group;

	for (unsigned int i = 0; i < RCAR_VIN_NUM; i++) {
		if (group->vin[i])
			rvin_v4l2_unregister(group->vin[i]);
	}

	mutex_lock(&vin->group->lock);

	for (unsigned int i = 0; i < RCAR_VIN_NUM; i++) {
		if (!group->vin[i] || group->vin[i]->parallel.asc != asc)
			continue;

		group->vin[i]->parallel.subdev = NULL;

		vin_dbg(group->vin[i], "Unbind parallel subdev %s\n",
			subdev->name);
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(group->remotes); i++) {
		if (group->remotes[i].asc != asc)
			continue;

		group->remotes[i].subdev = NULL;

		vin_dbg(vin, "Unbind %s from slot %u\n", subdev->name, i);
	}

	mutex_unlock(&vin->group->lock);

	media_device_unregister(&vin->group->mdev);
}

static int rvin_group_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_connection *asc)
{
	struct rvin_dev *vin = v4l2_dev_to_vin(notifier->v4l2_dev);
	struct rvin_group *group = vin->group;

	guard(mutex)(&group->lock);

	for (unsigned int i = 0; i < RCAR_VIN_NUM; i++) {
		struct rvin_dev *pvin = group->vin[i];

		if (!pvin || pvin->parallel.asc != asc)
			continue;

		pvin->parallel.source_pad = 0;
		for (unsigned int pad = 0; pad < subdev->entity.num_pads; pad++)
			if (subdev->entity.pads[pad].flags & MEDIA_PAD_FL_SOURCE)
				pvin->parallel.source_pad = pad;

		pvin->parallel.subdev = subdev;
		vin_dbg(pvin, "Bound subdev %s\n", subdev->name);

		return 0;
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(group->remotes); i++) {
		if (vin->group->remotes[i].asc != asc)
			continue;

		vin->group->remotes[i].subdev = subdev;
		vin_dbg(vin, "Bound %s to slot %u\n", subdev->name, i);

		return 0;
	}

	return -ENODEV;
}

static const struct v4l2_async_notifier_operations rvin_group_notify_ops = {
	.bound = rvin_group_notify_bound,
	.unbind = rvin_group_notify_unbind,
	.complete = rvin_group_notify_complete,
};

static int rvin_group_parse_of(struct rvin_dev *vin, unsigned int port,
			       unsigned int id)
{
	struct fwnode_handle *ep, *fwnode;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct v4l2_async_connection *asc;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(vin->dev), port, id, 0);
	if (!ep)
		return 0;

	fwnode = fwnode_graph_get_remote_endpoint(ep);
	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	fwnode_handle_put(ep);
	if (ret) {
		vin_err(vin, "Failed to parse %pOF\n", to_of_node(fwnode));
		ret = -EINVAL;
		goto out;
	}

	asc = v4l2_async_nf_add_fwnode(&vin->group->notifier, fwnode,
				       struct v4l2_async_connection);
	if (IS_ERR(asc)) {
		ret = PTR_ERR(asc);
		goto out;
	}

	vin->group->remotes[vep.base.id].asc = asc;

	vin_dbg(vin, "Add group OF device %pOF to slot %u\n",
		to_of_node(fwnode), vep.base.id);
out:
	fwnode_handle_put(fwnode);

	return ret;
}

static int rvin_parallel_parse_of(struct rvin_dev *vin)
{
	struct fwnode_handle *fwnode __free(fwnode_handle) = NULL;
	struct fwnode_handle *ep __free(fwnode_handle) = NULL;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_UNKNOWN,
	};
	struct v4l2_async_connection *asc;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(vin->dev), 0, 0, 0);
	if (!ep)
		return 0;

	if (v4l2_fwnode_endpoint_parse(ep, &vep)) {
		vin_err(vin, "Failed to parse %pOF\n", to_of_node(ep));
		return -EINVAL;
	}

	switch (vep.bus_type) {
	case V4L2_MBUS_PARALLEL:
	case V4L2_MBUS_BT656:
		vin_dbg(vin, "Found %s media bus\n",
			vep.bus_type == V4L2_MBUS_PARALLEL ?
			"PARALLEL" : "BT656");
		vin->parallel.mbus_type = vep.bus_type;
		vin->parallel.bus = vep.bus.parallel;
		break;
	default:
		vin_err(vin, "Unknown media bus type\n");
		return -EINVAL;
	}

	fwnode = fwnode_graph_get_remote_endpoint(ep);
	asc = v4l2_async_nf_add_fwnode(&vin->group->notifier, fwnode,
				       struct v4l2_async_connection);
	if (IS_ERR(asc))
		return PTR_ERR(asc);

	vin->parallel.asc = asc;

	vin_dbg(vin, "Add parallel OF device %pOF\n", to_of_node(fwnode));

	return 0;
}

static int rvin_group_notifier_init(struct rvin_dev *vin, unsigned int port,
				    unsigned int max_id)
{
	unsigned int count = 0, vin_mask = 0;
	unsigned int i, id;
	int ret;

	mutex_lock(&vin->group->lock);

	/* If not all VIN's are registered don't register the notifier. */
	for (i = 0; i < RCAR_VIN_NUM; i++) {
		if (vin->group->vin[i]) {
			count++;
			vin_mask |= BIT(i);
		}
	}

	if (vin->group->count != count) {
		mutex_unlock(&vin->group->lock);
		return 0;
	}

	mutex_unlock(&vin->group->lock);

	v4l2_async_nf_init(&vin->group->notifier, &vin->v4l2_dev);

	/*
	 * Some subdevices may overlap but the parser function can handle it and
	 * each subdevice will only be registered once with the group notifier.
	 */
	for (i = 0; i < RCAR_VIN_NUM; i++) {
		if (!(vin_mask & BIT(i)))
			continue;

		/* Parse local subdevice. */
		ret = rvin_parallel_parse_of(vin->group->vin[i]);
		if (ret)
			return ret;

		/* Parse shared subdevices. */
		for (id = 0; id < max_id; id++) {
			if (vin->group->remotes[id].asc)
				continue;

			ret = rvin_group_parse_of(vin->group->vin[i], port, id);
			if (ret)
				return ret;
		}
	}

	if (list_empty(&vin->group->notifier.waiting_list))
		return 0;

	vin->group->notifier.ops = &rvin_group_notify_ops;
	ret = v4l2_async_nf_register(&vin->group->notifier);
	if (ret < 0) {
		vin_err(vin, "Notifier registration failed\n");
		v4l2_async_nf_cleanup(&vin->group->notifier);
		return ret;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Controls
 */

static int rvin_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rvin_dev *vin =
		container_of(ctrl->handler, struct rvin_dev, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_ALPHA_COMPONENT:
		rvin_set_alpha(vin, ctrl->val);
		break;
	}

	return 0;
}

static const struct v4l2_ctrl_ops rvin_ctrl_ops = {
	.s_ctrl = rvin_s_ctrl,
};

static void rvin_free_controls(struct rvin_dev *vin)
{
	v4l2_ctrl_handler_free(&vin->ctrl_handler);
	vin->vdev.ctrl_handler = NULL;
}

static int rvin_create_controls(struct rvin_dev *vin)
{
	int ret;

	ret = v4l2_ctrl_handler_init(&vin->ctrl_handler, 1);
	if (ret < 0)
		return ret;

	/* The VIN directly deals with alpha component. */
	v4l2_ctrl_new_std(&vin->ctrl_handler, &rvin_ctrl_ops,
			  V4L2_CID_ALPHA_COMPONENT, 0, 255, 1, 255);

	if (vin->ctrl_handler.error) {
		ret = vin->ctrl_handler.error;
		rvin_free_controls(vin);
		return ret;
	}

	vin->vdev.ctrl_handler = &vin->ctrl_handler;

	return 0;
}

/* -----------------------------------------------------------------------------
 * CSI-2
 */

/*
 * Link setup for the links between a VIN and a CSI-2 receiver is a bit
 * complex. The reason for this is that the register controlling routing
 * is not present in each VIN instance. There are special VINs which
 * control routing for themselves and other VINs. There are not many
 * different possible links combinations that can be enabled at the same
 * time, therefor all already enabled links which are controlled by a
 * master VIN need to be taken into account when making the decision
 * if a new link can be enabled or not.
 *
 * 1. Find out which VIN the link the user tries to enable is connected to.
 * 2. Lookup which master VIN controls the links for this VIN.
 * 3. Start with a bitmask with all bits set.
 * 4. For each previously enabled link from the master VIN bitwise AND its
 *    route mask (see documentation for mask in struct rvin_group_route)
 *    with the bitmask.
 * 5. Bitwise AND the mask for the link the user tries to enable to the bitmask.
 * 6. If the bitmask is not empty at this point the new link can be enabled
 *    while keeping all previous links enabled. Update the CHSEL value of the
 *    master VIN and inform the user that the link could be enabled.
 *
 * Please note that no link can be enabled if any VIN in the group is
 * currently open.
 */
static int rvin_csi2_link_notify(struct media_link *link, u32 flags,
				 unsigned int notification)
{
	struct rvin_group *group = container_of(link->graph_obj.mdev,
						struct rvin_group, mdev);
	struct media_entity *entity;
	struct video_device *vdev;
	struct rvin_dev *vin;
	unsigned int i;
	int csi_id, ret;

	ret = v4l2_pipeline_link_notify(link, flags, notification);
	if (ret)
		return ret;

	/* Only care about link enablement for VIN nodes. */
	if (!(flags & MEDIA_LNK_FL_ENABLED) ||
	    !is_media_entity_v4l2_video_device(link->sink->entity))
		return 0;

	/*
	 * Don't allow link changes if any stream in the graph is active as
	 * modifying the CHSEL register fields can disrupt running streams.
	 */
	media_device_for_each_entity(entity, &group->mdev)
		if (media_entity_is_streaming(entity))
			return -EBUSY;

	/* Find the master VIN that controls the routes. */
	vdev = media_entity_to_video_device(link->sink->entity);
	vin = container_of(vdev, struct rvin_dev, vdev);

	mutex_lock(&group->lock);

	csi_id = rvin_group_entity_to_remote_id(group, link->source->entity);
	if (csi_id == -ENODEV) {
		struct v4l2_subdev *sd;

		/*
		 * Make sure the source entity subdevice is registered as
		 * a parallel input of one of the enabled VINs if it is not
		 * one of the CSI-2 subdevices.
		 *
		 * No hardware configuration required for parallel inputs,
		 * we can return here.
		 */
		sd = media_entity_to_v4l2_subdev(link->source->entity);
		for (i = 0; i < RCAR_VIN_NUM; i++) {
			if (group->vin[i] &&
			    group->vin[i]->parallel.subdev == sd) {
				group->vin[i]->is_csi = false;
				ret = 0;
				goto out;
			}
		}

		vin_err(vin, "Subdevice %s not registered to any VIN\n",
			link->source->entity->name);
		ret = -ENODEV;
	} else {
		const struct rvin_group_route *route;
		unsigned int chsel = UINT_MAX;
		unsigned int master_id;

		master_id = rvin_group_id_to_master(vin->id);

		if (WARN_ON(!group->vin[master_id])) {
			ret = -ENODEV;
			goto out;
		}

		/* Make sure group is connected to same CSI-2 */
		for (i = master_id; i < master_id + 4; i++) {
			struct media_pad *csi_pad;

			if (!group->vin[i])
				continue;

			/* Get remote CSI-2, if any. */
			csi_pad = media_pad_remote_pad_first(
					&group->vin[i]->vdev.entity.pads[0]);
			if (!csi_pad)
				continue;

			if (csi_pad->entity != link->source->entity) {
				vin_dbg(vin, "Already attached to %s\n",
					csi_pad->entity->name);
				ret = -EBUSY;
				goto out;
			}
		}

		for (route = vin->info->routes; route->chsel; route++) {
			if (route->master == master_id && route->csi == csi_id) {
				chsel = route->chsel;
				break;
			}
		}

		if (chsel == UINT_MAX) {
			vin_err(vin, "No CHSEL value found\n");
			ret = -EINVAL;
			goto out;
		}

		ret = rvin_set_channel_routing(group->vin[master_id], chsel);
		if (ret)
			goto out;

		vin->is_csi = true;
	}
out:
	mutex_unlock(&group->lock);

	return ret;
}

static const struct media_device_ops rvin_csi2_media_ops = {
	.link_notify = rvin_csi2_link_notify,
};

static int rvin_csi2_create_link(struct rvin_group *group, unsigned int id,
				 const struct rvin_group_route *route)
{
	struct media_entity *source = &group->remotes[route->csi].subdev->entity;
	struct media_entity *sink = &group->vin[id]->vdev.entity;
	struct media_pad *sink_pad = &sink->pads[0];
	unsigned int channel;
	int ret;

	for (channel = 0; channel < 4; channel++) {
		unsigned int source_idx = rvin_group_csi_channel_to_pad(channel);
		struct media_pad *source_pad = &source->pads[source_idx];

		/* Skip if link already exists. */
		if (media_entity_find_link(source_pad, sink_pad))
			continue;

		ret = media_create_pad_link(source, source_idx, sink, 0, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static int rvin_parallel_setup_links(struct rvin_group *group)
{
	u32 flags = MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE;

	guard(mutex)(&group->lock);

	/* If the group also has links don't enable the link. */
	for (unsigned int i = 0; i < ARRAY_SIZE(group->remotes); i++) {
		if (group->remotes[i].subdev) {
			flags = 0;
			break;
		}
	}

	/* Create links. */
	for (unsigned int i = 0; i < RCAR_VIN_NUM; i++) {
		struct rvin_dev *vin = group->vin[i];
		struct media_entity *source;
		struct media_entity *sink;
		int ret;

		/* Nothing to do if there is no VIN or parallel subdev. */
		if (!vin || !vin->parallel.subdev)
			continue;

		source = &vin->parallel.subdev->entity;
		sink = &vin->vdev.entity;

		ret = media_create_pad_link(source, vin->parallel.source_pad,
					    sink, 0, flags);
		if (ret)
			return ret;
	}

	return 0;
}

static int rvin_csi2_setup_links(struct rvin_group *group)
{
	const struct rvin_group_route *route;
	unsigned int id;
	int ret;

	ret = rvin_parallel_setup_links(group);
	if (ret)
		return ret;

	/* Create all media device links between VINs and CSI-2's. */
	mutex_lock(&group->lock);
	for (route = group->info->routes; route->chsel; route++) {
		/* Check that VIN' master is part of the group. */
		if (!group->vin[route->master])
			continue;

		/* Check that CSI-2 is part of the group. */
		if (!group->remotes[route->csi].subdev)
			continue;

		for (id = route->master; id < route->master + 4; id++) {
			/* Check that VIN is part of the group. */
			if (!group->vin[id])
				continue;

			ret = rvin_csi2_create_link(group, id, route);
			if (ret)
				goto out;
		}
	}
out:
	mutex_unlock(&group->lock);

	return ret;
}

static int rvin_csi2_init(struct rvin_dev *vin)
{
	int ret;

	ret = rvin_group_get(vin, rvin_csi2_setup_links, &rvin_csi2_media_ops);
	if (ret)
		return ret;

	ret = rvin_group_notifier_init(vin, 1, RVIN_CSI_MAX);
	if (ret)
		rvin_group_put(vin);

	return ret;
}

/* -----------------------------------------------------------------------------
 * ISP
 */

static int rvin_isp_setup_links(struct rvin_group *group)
{
	unsigned int i;
	int ret = -EINVAL;

	/* Create all media device links between VINs and ISP's. */
	mutex_lock(&group->lock);
	for (i = 0; i < RCAR_VIN_NUM; i++) {
		struct media_pad *source_pad, *sink_pad;
		struct media_entity *source, *sink;
		unsigned int source_slot = i / 8;
		unsigned int source_idx = i % 8 + 1;
		struct rvin_dev *vin = group->vin[i];

		if (!vin)
			continue;

		/* Check that ISP is part of the group. */
		if (!group->remotes[source_slot].subdev)
			continue;

		source = &group->remotes[source_slot].subdev->entity;
		source_pad = &source->pads[source_idx];

		sink = &vin->vdev.entity;
		sink_pad = &sink->pads[0];

		/* Skip if link already exists. */
		if (media_entity_find_link(source_pad, sink_pad))
			continue;

		ret = media_create_pad_link(source, source_idx, sink, 0,
					    MEDIA_LNK_FL_ENABLED |
					    MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			vin_err(vin, "Error adding link from %s to %s\n",
				source->name, sink->name);
			break;
		}
	}
	mutex_unlock(&group->lock);

	return ret;
}

static int rvin_isp_init(struct rvin_dev *vin)
{
	int ret;

	ret = rvin_group_get(vin, rvin_isp_setup_links, NULL);
	if (ret)
		return ret;

	ret = rvin_group_notifier_init(vin, 2, RVIN_ISP_MAX);
	if (ret)
		rvin_group_put(vin);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Suspend / Resume
 */

static int __maybe_unused rvin_suspend(struct device *dev)
{
	struct rvin_dev *vin = dev_get_drvdata(dev);

	if (!vin->running)
		return 0;

	rvin_stop_streaming(vin);

	return 0;
}

static int __maybe_unused rvin_resume(struct device *dev)
{
	struct rvin_dev *vin = dev_get_drvdata(dev);

	if (!vin->running)
		return 0;

	/*
	 * Restore group master CHSEL setting.
	 *
	 * This needs to be done by every VIN resuming not only the master
	 * as we don't know if and in which order the master VINs will
	 * be resumed.
	 */
	if (vin->info->model == RCAR_GEN3) {
		unsigned int master_id = rvin_group_id_to_master(vin->id);
		struct rvin_dev *master = vin->group->vin[master_id];
		int ret;

		if (WARN_ON(!master))
			return -ENODEV;

		ret = rvin_set_channel_routing(master, master->chsel);
		if (ret)
			return ret;
	}

	return rvin_start_streaming(vin);
}

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static const struct rvin_info rcar_info_h1 = {
	.model = RCAR_H1,
	.max_width = 2048,
	.max_height = 2048,
	.scaler = rvin_scaler_gen2,
};

static const struct rvin_info rcar_info_m1 = {
	.model = RCAR_M1,
	.max_width = 2048,
	.max_height = 2048,
	.scaler = rvin_scaler_gen2,
};

static const struct rvin_info rcar_info_gen2 = {
	.model = RCAR_GEN2,
	.max_width = 2048,
	.max_height = 2048,
	.scaler = rvin_scaler_gen2,
};

static const struct rvin_group_route rcar_info_r8a774e1_routes[] = {
	{ .master = 0, .csi = RVIN_CSI20, .chsel = 0x04 },
	{ .master = 0, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ .master = 4, .csi = RVIN_CSI20, .chsel = 0x04 },
	{ /* Sentinel */ }
};

static const struct rvin_info rcar_info_r8a774e1 = {
	.model = RCAR_GEN3,
	.max_width = 4096,
	.max_height = 4096,
	.routes = rcar_info_r8a774e1_routes,
};

static const struct rvin_group_route rcar_info_r8a7795_routes[] = {
	{ .master = 0, .csi = RVIN_CSI20, .chsel = 0x04 },
	{ .master = 0, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ .master = 4, .csi = RVIN_CSI20, .chsel = 0x04 },
	{ .master = 4, .csi = RVIN_CSI41, .chsel = 0x03 },
	{ /* Sentinel */ }
};

static const struct rvin_info rcar_info_r8a7795 = {
	.model = RCAR_GEN3,
	.nv12 = true,
	.max_width = 4096,
	.max_height = 4096,
	.routes = rcar_info_r8a7795_routes,
	.scaler = rvin_scaler_gen3,
};

static const struct rvin_group_route rcar_info_r8a7796_routes[] = {
	{ .master = 0, .csi = RVIN_CSI20, .chsel = 0x04 },
	{ .master = 0, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ .master = 4, .csi = RVIN_CSI20, .chsel = 0x04 },
	{ .master = 4, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ /* Sentinel */ }
};

static const struct rvin_info rcar_info_r8a7796 = {
	.model = RCAR_GEN3,
	.nv12 = true,
	.max_width = 4096,
	.max_height = 4096,
	.routes = rcar_info_r8a7796_routes,
	.scaler = rvin_scaler_gen3,
};

static const struct rvin_group_route rcar_info_r8a77965_routes[] = {
	{ .master = 0, .csi = RVIN_CSI20, .chsel = 0x04 },
	{ .master = 0, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ .master = 4, .csi = RVIN_CSI20, .chsel = 0x04 },
	{ .master = 4, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ /* Sentinel */ }
};

static const struct rvin_info rcar_info_r8a77965 = {
	.model = RCAR_GEN3,
	.nv12 = true,
	.max_width = 4096,
	.max_height = 4096,
	.routes = rcar_info_r8a77965_routes,
	.scaler = rvin_scaler_gen3,
};

static const struct rvin_group_route rcar_info_r8a77970_routes[] = {
	{ .master = 0, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ /* Sentinel */ }
};

static const struct rvin_info rcar_info_r8a77970 = {
	.model = RCAR_GEN3,
	.max_width = 4096,
	.max_height = 4096,
	.routes = rcar_info_r8a77970_routes,
};

static const struct rvin_group_route rcar_info_r8a77980_routes[] = {
	{ .master = 0, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ .master = 4, .csi = RVIN_CSI41, .chsel = 0x03 },
	{ /* Sentinel */ }
};

static const struct rvin_info rcar_info_r8a77980 = {
	.model = RCAR_GEN3,
	.nv12 = true,
	.max_width = 4096,
	.max_height = 4096,
	.routes = rcar_info_r8a77980_routes,
};

static const struct rvin_group_route rcar_info_r8a77990_routes[] = {
	{ .master = 4, .csi = RVIN_CSI40, .chsel = 0x03 },
	{ /* Sentinel */ }
};

static const struct rvin_info rcar_info_r8a77990 = {
	.model = RCAR_GEN3,
	.nv12 = true,
	.max_width = 4096,
	.max_height = 4096,
	.routes = rcar_info_r8a77990_routes,
	.scaler = rvin_scaler_gen3,
};

static const struct rvin_group_route rcar_info_r8a77995_routes[] = {
	{ /* Sentinel */ }
};

static const struct rvin_info rcar_info_r8a77995 = {
	.model = RCAR_GEN3,
	.nv12 = true,
	.max_width = 4096,
	.max_height = 4096,
	.routes = rcar_info_r8a77995_routes,
	.scaler = rvin_scaler_gen3,
};

static const struct rvin_info rcar_info_gen4 = {
	.model = RCAR_GEN4,
	.use_isp = true,
	.nv12 = true,
	.raw10 = true,
	.max_width = 4096,
	.max_height = 4096,
};

static const struct of_device_id rvin_of_id_table[] = {
	{
		.compatible = "renesas,vin-r8a774a1",
		.data = &rcar_info_r8a7796,
	},
	{
		.compatible = "renesas,vin-r8a774b1",
		.data = &rcar_info_r8a77965,
	},
	{
		.compatible = "renesas,vin-r8a774c0",
		.data = &rcar_info_r8a77990,
	},
	{
		.compatible = "renesas,vin-r8a774e1",
		.data = &rcar_info_r8a774e1,
	},
	{
		.compatible = "renesas,vin-r8a7778",
		.data = &rcar_info_m1,
	},
	{
		.compatible = "renesas,vin-r8a7779",
		.data = &rcar_info_h1,
	},
	{
		.compatible = "renesas,rcar-gen2-vin",
		.data = &rcar_info_gen2,
	},
	{
		.compatible = "renesas,vin-r8a7795",
		.data = &rcar_info_r8a7795,
	},
	{
		.compatible = "renesas,vin-r8a7796",
		.data = &rcar_info_r8a7796,
	},
	{
		.compatible = "renesas,vin-r8a77961",
		.data = &rcar_info_r8a7796,
	},
	{
		.compatible = "renesas,vin-r8a77965",
		.data = &rcar_info_r8a77965,
	},
	{
		.compatible = "renesas,vin-r8a77970",
		.data = &rcar_info_r8a77970,
	},
	{
		.compatible = "renesas,vin-r8a77980",
		.data = &rcar_info_r8a77980,
	},
	{
		.compatible = "renesas,vin-r8a77990",
		.data = &rcar_info_r8a77990,
	},
	{
		.compatible = "renesas,vin-r8a77995",
		.data = &rcar_info_r8a77995,
	},
	{
		/* Keep to be compatible with old DTS files. */
		.compatible = "renesas,vin-r8a779a0",
		.data = &rcar_info_gen4,
	},
	{
		/* Keep to be compatible with old DTS files. */
		.compatible = "renesas,vin-r8a779g0",
		.data = &rcar_info_gen4,
	},
	{
		.compatible = "renesas,rcar-gen4-vin",
		.data = &rcar_info_gen4,
	},
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, rvin_of_id_table);

static int rvin_id_get(struct rvin_dev *vin)
{
	u32 oid;
	int id;

	switch (vin->info->model) {
	case RCAR_GEN3:
	case RCAR_GEN4:
		if (of_property_read_u32(vin->dev->of_node, "renesas,id", &oid)) {
			vin_err(vin, "%pOF: No renesas,id property found\n",
				vin->dev->of_node);
			return -EINVAL;
		}

		if (oid < 0 || oid >= RCAR_VIN_NUM) {
			vin_err(vin, "%pOF: Invalid renesas,id '%u'\n",
				vin->dev->of_node, oid);
			return -EINVAL;
		}

		vin->id = oid;
		break;
	default:
		id = ida_alloc_range(&rvin_ida, 0, RCAR_VIN_NUM - 1,
				     GFP_KERNEL);
		if (id < 0) {
			vin_err(vin, "%pOF: Failed to allocate VIN group ID\n",
				vin->dev->of_node);
			return -EINVAL;
		}

		vin->id = id;
		break;
	}

	return 0;
}

static void rvin_id_put(struct rvin_dev *vin)
{
	switch (vin->info->model) {
	case RCAR_GEN3:
	case RCAR_GEN4:
		break;
	default:
		ida_free(&rvin_ida, vin->id);
		break;
	}
}

static int rcar_vin_probe(struct platform_device *pdev)
{
	struct rvin_dev *vin;
	int irq, ret;

	vin = devm_kzalloc(&pdev->dev, sizeof(*vin), GFP_KERNEL);
	if (!vin)
		return -ENOMEM;

	vin->dev = &pdev->dev;
	vin->info = of_device_get_match_data(&pdev->dev);
	vin->alpha = 0xff;

	vin->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vin->base))
		return PTR_ERR(vin->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = rvin_dma_register(vin, irq);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, vin);

	if (rvin_id_get(vin)) {
		ret = -EINVAL;
		goto err_dma;
	}

	vin->pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vin->vdev.entity, 1, &vin->pad);
	if (ret)
		goto err_id;

	ret = rvin_create_controls(vin);
	if (ret < 0)
		goto err_id;

	switch (vin->info->model) {
	case RCAR_GEN3:
	case RCAR_GEN4:
		if (vin->info->use_isp) {
			ret = rvin_isp_init(vin);
		} else {
			ret = rvin_csi2_init(vin);

			if (vin->info->scaler &&
			    rvin_group_id_to_master(vin->id) == vin->id)
				vin->scaler = vin->info->scaler;
		}
		break;
	default:
		ret = rvin_group_get(vin, rvin_parallel_setup_links, NULL);
		if (!ret)
			ret = rvin_group_notifier_init(vin, 0, 0);

		if (vin->info->scaler)
			vin->scaler = vin->info->scaler;
		break;
	}

	if (ret)
		goto err_ctrl;

	pm_suspend_ignore_children(&pdev->dev, true);
	pm_runtime_enable(&pdev->dev);

	return 0;

err_ctrl:
	rvin_free_controls(vin);
err_id:
	rvin_id_put(vin);
err_dma:
	rvin_dma_unregister(vin);

	return ret;
}

static void rcar_vin_remove(struct platform_device *pdev)
{
	struct rvin_dev *vin = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);

	rvin_v4l2_unregister(vin);

	if (&vin->v4l2_dev == vin->group->notifier.v4l2_dev) {
		v4l2_async_nf_unregister(&vin->group->notifier);
		v4l2_async_nf_cleanup(&vin->group->notifier);
	}

	rvin_group_put(vin);

	rvin_free_controls(vin);

	rvin_id_put(vin);

	rvin_dma_unregister(vin);
}

static SIMPLE_DEV_PM_OPS(rvin_pm_ops, rvin_suspend, rvin_resume);

static struct platform_driver rcar_vin_driver = {
	.driver = {
		.name = "rcar-vin",
		.suppress_bind_attrs = true,
		.pm = &rvin_pm_ops,
		.of_match_table = rvin_of_id_table,
	},
	.probe = rcar_vin_probe,
	.remove = rcar_vin_remove,
};

module_platform_driver(rcar_vin_driver);

MODULE_AUTHOR("Niklas Söderlund <niklas.soderlund@ragnatech.se>");
MODULE_DESCRIPTION("Renesas R-Car VIN camera host driver");
MODULE_LICENSE("GPL");
