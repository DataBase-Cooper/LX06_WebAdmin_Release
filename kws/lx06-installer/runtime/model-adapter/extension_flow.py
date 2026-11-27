"""Script-owned UI steps; the Web/ROM layer never recognizes provider names.

Use the same field/action registry as validation. A step does not grant access:
the script must still verify credentials before any provider operation.
"""

def step(id, title, fields=(), actions=(), **kwargs):
    return dict(id=id, title=title, fields=list(fields), actions=list(actions), **kwargs)


def account_flow(schema, provider):
    login_fields = [f['name'] for f in schema['fields'] if f['name'] in
                    ('account', 'sms_code', 'password', 'area_code')]
    config_fields = [f['name'] for f in schema['fields'] if f['name'] not in
                     (*login_fields, 'auth_callback', 'question')]
    ids = {a['id'] for a in schema['actions']}
    login_actions = [a for a in ('login', 'send_sms', 'sms_login', 'website') if a in ids]
    steps = [step('login', '登录账户', login_fields, login_actions,
                  description='先登录账户，成功后显示可用模型与对话设置。')]
    if provider == 'chatgpt':
        steps.append(step('authorize', '完成授权', ['auth_callback'], ['complete-login', 'login'],
                          description='在官网授权后，将跳转地址粘贴到此处完成登录。', back='login'))
    elif provider == 'grok':
        steps.append(step('authorize', '官网授权', description='打开授权页面输入验证码，完成后会自动进入配置。'))
    config_actions = [a for a in ('save', 'models', 'regenerate-greeting', 'status', 'logout') if a in ids]
    # Text testing is optional and separate from the short everyday form.
    if 'question' in [f['name'] for f in schema['fields']]:
        config_fields += ['question']
        config_actions += ['test'] if 'test' in ids else []
    steps.append(step('configure', '对话设置', config_fields, config_actions,
                      description='保存后下次对话生效。账户额度以平台页面为准。'))
    if provider == 'deepseek' and not config_fields:
        steps[-1]['back'] = 'login'
        steps[0]['actions'] += ['status']
    schema['flow'] = dict(initial='login', steps=steps)
    return schema
