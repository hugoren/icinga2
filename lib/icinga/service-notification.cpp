/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "i2-icinga.h"

using namespace icinga;

boost::mutex Service::m_NotificationMutex;
map<String, set<Notification::WeakPtr> > Service::m_NotificationsCache;
bool Service::m_NotificationsCacheNeedsUpdate = false;
Timer::Ptr Service::m_NotificationsCacheTimer;

/**
 * @threadsafety Always.
 */
void Service::RequestNotifications(NotificationType type)
{
	{
		ObjectLock olock(this);
		SetLastNotification(Utility::GetTime());
	}

	RequestMessage msg;
	msg.SetMethod("icinga::SendNotifications");

	NotificationRequestMessage params;
	msg.SetParams(params);

	params.SetService(GetName());
	params.SetType(type);

	Logger::Write(LogDebug, "icinga", "Sending notification anycast request for service '" + GetName() + "'");
	EndpointManager::GetInstance()->SendAnycastMessage(Endpoint::Ptr(), msg);
}

/**
 * @threadsafety Always.
 */
void Service::SendNotifications(NotificationType type)
{
	if (!GetEnableNotifications()) {
		Logger::Write(LogInformation, "icinga", "Notifications are disabled for service '" + GetName() + "'.");
		return;
	}

	Logger::Write(LogInformation, "icinga", "Sending notifications for service '" + GetName() + "'");

	set<Notification::Ptr> notifications = GetNotifications();

	if (notifications.size() == 0)
		Logger::Write(LogInformation, "icinga", "Service '" + GetName() + "' does not have any notifications.");

	BOOST_FOREACH(const Notification::Ptr& notification, notifications) {
		try {
			notification->BeginExecuteNotification(type);
		} catch (const exception& ex) {
			stringstream msgbuf;
			msgbuf << "Exception occured during notification for service '"
			       << GetName() << "': " << diagnostic_information(ex);
			String message = msgbuf.str();

			Logger::Write(LogWarning, "icinga", message);
		}
	}
}

/**
 * @threadsafety Always.
 */
void Service::InvalidateNotificationsCache(void)
{
	boost::mutex::scoped_lock lock(m_NotificationMutex);

	if (m_NotificationsCacheNeedsUpdate)
		return; /* Someone else has already requested a refresh. */

	if (!m_NotificationsCacheTimer) {
		m_NotificationsCacheTimer = boost::make_shared<Timer>();
		m_NotificationsCacheTimer->SetInterval(0.5);
		m_NotificationsCacheTimer->OnTimerExpired.connect(boost::bind(&Service::RefreshNotificationsCache));
	}

	m_NotificationsCacheTimer->Start();
	m_NotificationsCacheNeedsUpdate = true;
}

/**
 * @threadsafety Always.
 */
void Service::RefreshNotificationsCache(void)
{
	{
		boost::mutex::scoped_lock lock(m_NotificationMutex);

		assert(m_NotificationsCacheNeedsUpdate);
		m_NotificationsCacheTimer->Stop();
		m_NotificationsCacheNeedsUpdate = false;
	}

	map<String, set<Notification::WeakPtr> > newNotificationsCache;

	BOOST_FOREACH(const DynamicObject::Ptr& object, DynamicType::GetObjects("Notification")) {
		const Notification::Ptr& notification = static_pointer_cast<Notification>(object);

		Service::Ptr service = notification->GetService();

		if (!service)
			continue;

		newNotificationsCache[service->GetName()].insert(notification);
	}

	boost::mutex::scoped_lock lock(m_NotificationMutex);
	m_NotificationsCache.swap(newNotificationsCache);
}

/**
 * @threadsafety Always.
 */
set<Notification::Ptr> Service::GetNotifications(void) const
{
	set<Notification::Ptr> notifications;

	{
		boost::mutex::scoped_lock lock(m_NotificationMutex);

		BOOST_FOREACH(const Notification::WeakPtr& wservice, m_NotificationsCache[GetName()]) {
			Notification::Ptr notification = wservice.lock();

			if (!notification)
				continue;

			notifications.insert(notification);
		}
	}

	return notifications;
}

/**
 * @threadsafety Always.
 */
template<typename TDict>
static void CopyNotificationAttributes(TDict notificationDesc, const ConfigItemBuilder::Ptr& builder)
{
	/* TODO: we only need to copy macros if this is an inline definition,
	 * i.e. "typeid(notificationDesc)" != Notification, however for now we just
	 * copy them anyway. */
	Value macros = notificationDesc->Get("macros");
	if (!macros.IsEmpty())
		builder->AddExpression("macros", OperatorPlus, macros);

	Value users = notificationDesc->Get("users");
	if (!users.IsEmpty())
		builder->AddExpression("users", OperatorPlus, users);

	Value groups = notificationDesc->Get("groups");
	if (!groups.IsEmpty())
		builder->AddExpression("groups", OperatorPlus, groups);

	/*Value notificationInterval = notificationDesc->Get("notification_interval");
	if (!notificationInterval.IsEmpty())
		builder->AddExpression("notification_interval", OperatorSet, notificationInterval);*/
}

void Service::UpdateSlaveNotifications(void)
{
	Dictionary::Ptr oldNotifications;
	vector<Dictionary::Ptr> notificationDescsList;
	ConfigItem::Ptr item;

	item = ConfigItem::GetObject("Service", GetName());

	/* Don't create slave notifications unless we own this object
	 * and it's not a template. */
	if (!item || IsAbstract())
		return;

	{
		ObjectLock olock(this);
		oldNotifications = m_SlaveNotifications;
	}

	notificationDescsList.push_back(Get("notifications"));

	Dictionary::Ptr newNotifications;
	newNotifications = boost::make_shared<Dictionary>();

	Host::Ptr host = GetHost();

	if (!host)
		return;

	notificationDescsList.push_back(host->Get("notifications"));

	BOOST_FOREACH(const Dictionary::Ptr& notificationDescs, notificationDescsList) {
		if (!notificationDescs)
			continue;

		ObjectLock olock(notificationDescs);

		String nfcname;
		Value nfcdesc;
		BOOST_FOREACH(tie(nfcname, nfcdesc), notificationDescs) {
			if (nfcdesc.IsScalar())
				nfcname = nfcdesc;

			stringstream namebuf;
			namebuf << GetName() << "-" << nfcname;
			String name = namebuf.str();

			ConfigItemBuilder::Ptr builder = boost::make_shared<ConfigItemBuilder>(item->GetDebugInfo());
			builder->SetType("Notification");
			builder->SetName(name);
			builder->AddExpression("host_name", OperatorSet, host->GetName());
			builder->AddExpression("service", OperatorSet, GetShortName());

			CopyNotificationAttributes(this, builder);

			if (nfcdesc.IsScalar()) {
				builder->AddParent(nfcdesc);
			} else if (nfcdesc.IsObjectType<Dictionary>()) {
				Dictionary::Ptr notification = nfcdesc;

				Dictionary::Ptr templates = notification->Get("templates");

				if (templates) {
					ObjectLock tlock(templates);

					String tmpl;
					BOOST_FOREACH(tie(tuples::ignore, tmpl), templates) {
						builder->AddParent(tmpl);
					}
				} else {
					builder->AddParent(nfcname);
				}

				CopyNotificationAttributes(notification, builder);
			} else {
				BOOST_THROW_EXCEPTION(invalid_argument("Notification description must be either a string or a dictionary."));
			}

			ConfigItem::Ptr notificationItem = builder->Compile();
			notificationItem->Commit();

			newNotifications->Set(name, notificationItem);
		}
	}

	if (oldNotifications) {
		ObjectLock olock(oldNotifications);

		ConfigItem::Ptr notification;
		BOOST_FOREACH(tie(tuples::ignore, notification), oldNotifications) {
			if (!notification)
				continue;

			if (!newNotifications->Contains(notification->GetName()))
				notification->Unregister();
		}
	}

	{
		ObjectLock olock(this);
		m_SlaveNotifications = newNotifications;
	}
}

/**
 * @threadsafety Always.
 */
double Service::GetLastNotification(void) const
{
	if (m_LastNotification.IsEmpty())
		return 0;
	else
		return m_LastNotification;
}

/**
 * @threadsafety Always.
 */
void Service::SetLastNotification(double time)
{
	m_LastNotification = time;
	Touch("last_notification");
}

/**
 * @threadsafety Always.
 */
bool Service::GetEnableNotifications(void) const
{
	if (m_EnableNotifications.IsEmpty())
		return true;
	else
		return m_EnableNotifications;
}

/**
 * @threadsafety Always.
 */
void Service::SetEnableNotifications(bool enabled)
{
	m_EnableNotifications = enabled;
	Touch("enable_notifications");
}

/**
 * @threadsafety Always.
 */
double Service::GetNotificationInterval(void) const
{
	if (m_NotificationInterval.IsEmpty())
		return 300;
	else
		return m_NotificationInterval;
}
