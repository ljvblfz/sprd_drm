/*
 * Hisilicon Kirin SoCs drm master driver
 *
 * Copyright (c) 2016 Linaro Limited.
 * Copyright (c) 2014-2016 Hisilicon Limited.
 *
 * Author:
 *	<cailiwei@hisilicon.com>
 *	<zhengwanchun@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/of_platform.h>
#include <linux/component.h>
#include <linux/of_graph.h>

#include <drm/drmP.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>

#include "sprd_drm_drv.h"


#ifdef CONFIG_DRM_FBDEV_EMULATION
static bool fbdev = true;
MODULE_PARM_DESC(fbdev, "Enable fbdev compat layer");
module_param(fbdev, bool, 0600);
#endif


static struct sprd_dc_ops *dc_ops;

static int sprd_drm_kms_cleanup(struct drm_device *dev)
{
	struct sprd_drm_private *priv = dev->dev_private;

	if (priv->fbdev) {
		sprd_drm_fbdev_fini(dev);
		priv->fbdev = NULL;
	}

	drm_kms_helper_poll_fini(dev);
	drm_vblank_cleanup(dev);
	dc_ops->cleanup(dev);
	drm_mode_config_cleanup(dev);
	devm_kfree(dev->dev, priv);
	dev->dev_private = NULL;

	return 0;
}

static void sprd_fbdev_output_poll_changed(struct drm_device *dev)
{
	struct sprd_drm_private *priv = dev->dev_private;

	dsi_set_output_client(dev);

	if (priv->fbdev)
		drm_fb_helper_hotplug_event(priv->fbdev);
	else
		priv->fbdev = sprd_drm_fbdev_init(dev);
}

static const struct drm_mode_config_funcs sprd_drm_mode_config_funcs = {
	.fb_create = drm_fb_cma_create,
	.output_poll_changed = sprd_fbdev_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void sprd_drm_mode_config_init(struct drm_device *dev)
{
	drm_mode_config_init(dev);

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = 2048;
	dev->mode_config.max_height = 2048;

	dev->mode_config.funcs = &sprd_drm_mode_config_funcs;
}

static int sprd_drm_kms_init(struct drm_device *dev)
{
	struct sprd_drm_private *priv;
	int ret;

	priv = devm_kzalloc(dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev->dev_private = priv;
	dev_set_drvdata(dev->dev, dev);

	/* dev->mode_config initialization */
	sprd_drm_mode_config_init(dev);

	/* display controller init */
	ret = dc_ops->init(dev);
	if (ret)
		goto err_mode_config_cleanup;

	/* bind and init sub drivers */
	ret = component_bind_all(dev->dev, dev);
	if (ret) {
		DRM_ERROR("failed to bind all component.\n");
		goto err_dc_cleanup;
	}

	/* vblank init */
	ret = drm_vblank_init(dev, dev->mode_config.num_crtc);
	if (ret) {
		DRM_ERROR("failed to initialize vblank.\n");
		goto err_unbind_all;
	}
	/* with irq_enabled = true, we can use the vblank feature. */
	dev->irq_enabled = true;

	/* reset all the states of crtc/plane/encoder/connector */
	drm_mode_config_reset(dev);

	/* init kms poll for handling hpd */
	drm_kms_helper_poll_init(dev);

	/* force detection after connectors init */
	(void)drm_helper_hpd_irq_event(dev);

	return 0;

err_unbind_all:
	component_unbind_all(dev->dev, dev);
err_dc_cleanup:
	dc_ops->cleanup(dev);
err_mode_config_cleanup:
	drm_mode_config_cleanup(dev);
	devm_kfree(dev->dev, priv);
	dev->dev_private = NULL;

	return ret;
}

static const struct file_operations sprd_drm_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= no_llseek,
	.mmap		= drm_gem_cma_mmap,
};

static int sprd_drm_connectors_register(struct drm_device *dev)
{
	struct drm_connector *connector;
	struct drm_connector *failed_connector;
	int ret;

	mutex_lock(&dev->mode_config.mutex);
	drm_for_each_connector(connector, dev) {
		ret = drm_connector_register(connector);
		if (ret) {
			failed_connector = connector;
			goto err;
		}
	}
	mutex_unlock(&dev->mode_config.mutex);

	return 0;

err:
	drm_for_each_connector(connector, dev) {
		if (failed_connector == connector)
			break;
		drm_connector_unregister(connector);
	}
	mutex_unlock(&dev->mode_config.mutex);

	return ret;
}

static struct drm_driver sprd_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC | DRIVER_HAVE_IRQ | DRIVER_RENDER,
	.fops				= &sprd_drm_fops,
	.set_busid			= drm_platform_set_busid,

	.gem_free_object	= drm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.dumb_create		= drm_gem_cma_dumb_create_internal,
	.dumb_map_offset	= drm_gem_cma_dumb_map_offset,
	.dumb_destroy		= drm_gem_dumb_destroy,

	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_get_sg_table = drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,

	.name			= "sprd",
	.desc			= "Spreadtrum SoCs' DRM Driver",
	.date			= "20170309",
	.major			= 1,
	.minor			= 0,
};

#ifdef CONFIG_OF
/* NOTE: the CONFIG_OF case duplicates the same code as exynos or imx
 * (or probably any other).. so probably some room for some helpers
 */
static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}
#else
static int compare_dev(struct device *dev, void *data)
{
	return dev == data;
}
#endif

static int sprd_drm_bind(struct device *dev)
{
	struct drm_driver *driver = &sprd_drm_driver;
	struct drm_device *drm_dev;
	int ret;

	//drm_platform_init(&sprd_drm_driver, to_platform_device(dev));

	drm_dev = drm_dev_alloc(driver, dev);
	if (!drm_dev)
		return -ENOMEM;

	drm_dev->platformdev = to_platform_device(dev);

	ret = sprd_drm_kms_init(drm_dev);
	if (ret)
		goto err_drm_dev_unref;

	ret = drm_dev_register(drm_dev, 0);
	if (ret)
		goto err_kms_cleanup;

	/* connectors should be registered after drm device register */
	ret = sprd_drm_connectors_register(drm_dev);
	if (ret)
		goto err_drm_dev_unregister;

	DRM_INFO("Initialized %s %d.%d.%d %s on minor %d\n",
		 driver->name, driver->major, driver->minor, driver->patchlevel,
		 driver->date, drm_dev->primary->index);

	return 0;

err_drm_dev_unregister:
	drm_dev_unregister(drm_dev);
err_kms_cleanup:
	sprd_drm_kms_cleanup(drm_dev);
err_drm_dev_unref:
	drm_dev_unref(drm_dev);

	return ret;
}

static void sprd_drm_unbind(struct device *dev)
{
	drm_put_dev(dev_get_drvdata(dev));
}

static const struct component_master_ops sprd_drm_ops = {
	.bind = sprd_drm_bind,
	.unbind = sprd_drm_unbind,
};

static struct device_node *sprd_get_remote_node(struct device_node *np)
{
	struct device_node *endpoint, *remote;

	/* get the first endpoint, in our case only one remote node
	 * is connected to display controller.
	 */
	endpoint = of_graph_get_next_endpoint(np, NULL);
	if (!endpoint) {
		DRM_ERROR("no valid endpoint node\n");
		return ERR_PTR(-ENODEV);
	}
	of_node_put(endpoint);

	remote = of_graph_get_remote_port_parent(endpoint);
	if (!remote) {
		DRM_ERROR("no valid remote node\n");
		return ERR_PTR(-ENODEV);
	}
	of_node_put(remote);

	if (!of_device_is_available(remote)) {
		DRM_ERROR("not available for remote node\n");
		return ERR_PTR(-ENODEV);
	}

	return remote;
}

static int sprd_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct component_match *match = NULL;
	struct device_node *remote;

	dc_ops = (struct sprd_dc_ops *)of_device_get_match_data(dev);
	if (!dc_ops) {
		DRM_ERROR("failed to get dt id data\n");
		return -EINVAL;
	}

	remote = sprd_get_remote_node(np);
	if (IS_ERR(remote))
		return PTR_ERR(remote);

	component_match_add(dev, &match, compare_of, remote);

	return component_master_add_with_match(dev, &sprd_drm_ops, match);

	return 0;
}

static int sprd_drm_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &sprd_drm_ops);
	dc_ops = NULL;
	return 0;
}

static const struct of_device_id sprd_drm_dt_ids[] = {
	{ .compatible = "sprd-drm",
	  .data = &dss_dc_ops,
	},
	{ /* end node */ },
};
MODULE_DEVICE_TABLE(of, sprd_drm_dt_ids);

static struct platform_driver sprd_drm_driver = {
	.probe = sprd_drm_probe,
	.remove = sprd_drm_remove,
	.driver = {
		.name = "sprd-drm-drv",
		.of_match_table = sprd_drm_dt_ids,
	},
};

module_platform_driver(sprd_drm_driver);

MODULE_AUTHOR("Leon He <leon.he@unisoc.com>");
MODULE_DESCRIPTION("Unisoc SoCs' DRM master driver");
MODULE_LICENSE("GPL v2");
