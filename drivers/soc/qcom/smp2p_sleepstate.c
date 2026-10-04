// SPDX-License-Identifier: GPL-2.0-only
/*
 * Publish the application processor's sleep state to the ADSP over SMP2P.
 *
 * The ADSP's sensors framework (SEE) derives its remote_proc_state sensor
 * from the "sleepstate" SMP2P entry. Without it that sensor never appears,
 * and firmware that depends on it (e.g. Google's ASH on the Pixel 4a)
 * crashes the whole ADSP. Based on the downstream smp2p_sleepstate driver.
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/suspend.h>

#define AWAKE_BIT	BIT(12)

struct smp2p_sleepstate {
	struct qcom_smem_state *state;
	struct notifier_block pm_nb;
	struct wakeup_source *ws;
};

static int smp2p_sleepstate_pm_notifier(struct notifier_block *nb,
					unsigned long event, void *unused)
{
	struct smp2p_sleepstate *ss = container_of(nb, struct smp2p_sleepstate, pm_nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		qcom_smem_state_update_bits(ss->state, AWAKE_BIT, 0);
		/* Give the remote time to see it, as downstream does */
		usleep_range(10000, 10500);
		break;
	case PM_POST_SUSPEND:
		qcom_smem_state_update_bits(ss->state, AWAKE_BIT, AWAKE_BIT);
		break;
	}

	return NOTIFY_DONE;
}

static irqreturn_t smp2p_sleepstate_irq(int irq, void *data)
{
	struct smp2p_sleepstate *ss = data;

	/* The ADSP wants us awake for a moment */
	pm_wakeup_ws_event(ss->ws, 200, false);

	return IRQ_HANDLED;
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct smp2p_sleepstate *ss;
	int irq, ret;

	ss = devm_kzalloc(dev, sizeof(*ss), GFP_KERNEL);
	if (!ss)
		return -ENOMEM;

	ss->state = devm_qcom_smem_state_get(dev, NULL, &ret);
	if (IS_ERR(ss->state))
		return dev_err_probe(dev, PTR_ERR(ss->state), "no smem state\n");

	ss->ws = wakeup_source_register(dev, "smp2p-sleepstate");
	if (!ss->ws)
		return -ENOMEM;

	irq = platform_get_irq_byname(pdev, "smp2p-sleepstate-in");
	if (irq < 0) {
		ret = irq;
		goto err_ws;
	}

	ret = devm_request_threaded_irq(dev, irq, NULL, smp2p_sleepstate_irq,
					IRQF_ONESHOT, "smp2p_sleepstate", ss);
	if (ret)
		goto err_ws;

	ss->pm_nb.notifier_call = smp2p_sleepstate_pm_notifier;
	ss->pm_nb.priority = INT_MAX;
	ret = register_pm_notifier(&ss->pm_nb);
	if (ret)
		goto err_ws;

	platform_set_drvdata(pdev, ss);
	qcom_smem_state_update_bits(ss->state, AWAKE_BIT, AWAKE_BIT);

	return 0;

err_ws:
	wakeup_source_unregister(ss->ws);
	return ret;
}

static void smp2p_sleepstate_remove(struct platform_device *pdev)
{
	struct smp2p_sleepstate *ss = platform_get_drvdata(pdev);

	unregister_pm_notifier(&ss->pm_nb);
	wakeup_source_unregister(ss->ws);
}

static const struct of_device_id smp2p_sleepstate_of_match[] = {
	{ .compatible = "qcom,smp2p-sleepstate" },
	{}
};
MODULE_DEVICE_TABLE(of, smp2p_sleepstate_of_match);

static struct platform_driver smp2p_sleepstate_driver = {
	.probe = smp2p_sleepstate_probe,
	.remove = smp2p_sleepstate_remove,
	.driver = {
		.name = "smp2p-sleepstate",
		.of_match_table = smp2p_sleepstate_of_match,
	},
};
module_platform_driver(smp2p_sleepstate_driver);

MODULE_DESCRIPTION("Qualcomm SMP2P application processor sleep state");
MODULE_LICENSE("GPL");
