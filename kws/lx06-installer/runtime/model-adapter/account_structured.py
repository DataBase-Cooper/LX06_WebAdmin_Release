"""Declare transient model carriers after ordinary UI/settings are finalized."""
def account_structured(schema, *, request='request', prompt='prompt', response='response',
                       invocation='invocation', status='status', status_action='status', logged_in='logged_in'):
    # DeepSeek already owns ten UI actions. Keep the original twelve-action
    # budget: understand includes classification, and extract serves selected
    # intent fallback. Do not claim an independent intent-only adapter.
    for name,cap in [(request,8192),(prompt,2048),(response,8192),(invocation,64),(status,64)]:
        schema['fields'].append(dict(name=name,label=name,type='textarea',maxBytes=cap))
    status_entry=next((a for a in schema['actions'] if a['id']==status_action),None)
    if status_entry is None:
        status_entry=dict(id=status_action,label=status_action,fields=[])
        schema['actions'].append(status_entry)
    assert status_entry['fields']==[]
    status_entry['fields']=[invocation]
    for action in ('extract','understand'):
        schema['actions'].append(dict(id=action,label=action,fields=[request,prompt,invocation]))
    schema['structured']=dict(version=1,request_field=request,prompt_field=prompt,
        response_field=response,invocation_field=invocation,status_action=status_action,
        status_field=status,logged_in_value=logged_in,actions=['extract','understand'])
    assert len(schema['fields'])<=24 and len(schema['actions'])<=12
