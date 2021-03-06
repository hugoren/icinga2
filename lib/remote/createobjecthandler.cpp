/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2018 Icinga Development Team (https://icinga.com/)      *
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

#include "remote/createobjecthandler.hpp"
#include "remote/configobjectutility.hpp"
#include "remote/httputility.hpp"
#include "remote/jsonrpcconnection.hpp"
#include "remote/filterutility.hpp"
#include "remote/apiaction.hpp"
#include "remote/zone.hpp"
#include "base/configtype.hpp"
#include <set>

using namespace icinga;

REGISTER_URLHANDLER("/v1/objects", CreateObjectHandler);

bool CreateObjectHandler::HandleRequest(const ApiUser::Ptr& user, HttpRequest& request, HttpResponse& response, const Dictionary::Ptr& params)
{
	if (request.RequestUrl->GetPath().size() != 4)
		return false;

	if (request.RequestMethod != "PUT")
		return false;

	Type::Ptr type = FilterUtility::TypeFromPluralName(request.RequestUrl->GetPath()[2]);

	if (!type) {
		HttpUtility::SendJsonError(response, params, 400, "Invalid type specified.");
		return true;
	}

	FilterUtility::CheckPermission(user, "objects/create/" + type->GetName());

	String name = request.RequestUrl->GetPath()[3];
	Array::Ptr templates = params->Get("templates");
	Dictionary::Ptr attrs = params->Get("attrs");

	/* Put created objects into the local zone if not explicitly defined.
	 * This allows additional zone members to sync the
	 * configuration at some later point.
	 */
	Zone::Ptr localZone = Zone::GetLocalZone();
	String localZoneName;

	if (localZone) {
		localZoneName = localZone->GetName();

		if (!attrs) {
			attrs = new Dictionary({
				{ "zone", localZoneName }
			});
		} else if (!attrs->Contains("zone")) {
			attrs->Set("zone", localZoneName);
		}
	}

	/* Sanity checks for unique groups array. */
	if (attrs->Contains("groups")) {
		Array::Ptr groups = attrs->Get("groups");

		if (groups)
			attrs->Set("groups", groups->Unique());
	}

	Dictionary::Ptr result1 = new Dictionary();
	String status;
	Array::Ptr errors = new Array();
	Array::Ptr diagnosticInformation = new Array();

	bool ignoreOnError = false;

	if (params->Contains("ignore_on_error"))
		ignoreOnError = HttpUtility::GetLastParameter(params, "ignore_on_error");

	Dictionary::Ptr result = new Dictionary({
		{ "results", new Array({ result1 }) }
	});

	String config;

	bool verbose = false;

	if (params)
		verbose = HttpUtility::GetLastParameter(params, "verbose");

	/* Object creation can cause multiple errors and optionally diagnostic information.
	 * We can't use SendJsonError() here.
	 */
	try {
		config = ConfigObjectUtility::CreateObjectConfig(type, name, ignoreOnError, templates, attrs);
	} catch (const std::exception& ex) {
		errors->Add(DiagnosticInformation(ex, false));
		diagnosticInformation->Add(DiagnosticInformation(ex));

		if (verbose)
			result1->Set("diagnostic_information", diagnosticInformation);

		result1->Set("errors", errors);
		result1->Set("code", 500);
		result1->Set("status", "Object could not be created.");

		response.SetStatus(500, "Object could not be created");
		HttpUtility::SendJsonBody(response, params, result);

		return true;
	}

	if (!ConfigObjectUtility::CreateObject(type, name, config, errors, diagnosticInformation)) {
		result1->Set("errors", errors);
		result1->Set("code", 500);
		result1->Set("status", "Object could not be created.");

		if (verbose)
			result1->Set("diagnostic_information", diagnosticInformation);

		response.SetStatus(500, "Object could not be created");
		HttpUtility::SendJsonBody(response, params, result);

		return true;
	}

	auto *ctype = dynamic_cast<ConfigType *>(type.get());
	ConfigObject::Ptr obj = ctype->GetObject(name);

	result1->Set("code", 200);

	if (obj)
		result1->Set("status", "Object was created");
	else if (!obj && ignoreOnError)
		result1->Set("status", "Object was not created but 'ignore_on_error' was set to true");

	response.SetStatus(200, "OK");
	HttpUtility::SendJsonBody(response, params, result);

	return true;
}
